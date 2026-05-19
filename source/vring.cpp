/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include "vring.hpp"
#include <assert.h>
#include <unistd.h>
#include "Std_Debug.h"
#include <cinttypes>
#include <fcntl.h>

namespace as {
namespace vdds {
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_VRING 0
#define AS_LOG_VRINGI 1
#define AS_LOG_VRINGW 2
#define AS_LOG_VRINGE 3

#ifndef VRING_DESC_TIMEOUT
#define VRING_DESC_TIMEOUT (2000000)
#endif

#ifndef VRING_SPIN_MAX_COUNTER
#define VRING_SPIN_MAX_COUNTER (1000000)
#endif

/* ================================ [ TYPES     ] ============================================== */
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
/* ================================ [ LOCALS    ] ============================================== */
static std::string replace(std::string resource_str, std::string sub_str, std::string new_str) {
  std::string dst_str = resource_str;
  std::string::size_type pos = 0;
  while ((pos = dst_str.find(sub_str)) != std::string::npos) {
    dst_str.replace(pos, sub_str.length(), new_str);
  }
  return dst_str;
}

static std::string toAsName(std::string name) {
  std::string fname = "as" + replace(name, "/", "_");
  return fname;
}

/* ================================ [ FUNCTIONS ] ============================================== */
VRingBase::VRingBase(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc, size_t msgSize)
  : m_uioId(uioId), m_Name(name), m_isIvshmem(isIvshmem), m_numDesc(numDesc), m_MsgSize(msgSize) {
}

VRingBase::~VRingBase() {
}

uint32_t VRingBase::size() {
  uint32_t control_size = VRING_SIZE_OF_META(m_numDesc) + 
                        VRING_SIZE_OF_DESC(m_numDesc) + 
                        VRING_SIZE_OF_AVAIL(m_numDesc) + 
                        VRING_SIZE_OF_ALL_USED(m_numDesc);
                        
  uint32_t payload_size = (uint32_t)m_numDesc * m_MsgSize;
  
  return control_size + payload_size;
}

uint64_t VRingBase::timestamp() {
  uint64_t tsp = 0;
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  tsp = (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);

  return tsp;
}

/* https://rigtorp.se/spinlock/ */
int VRingBase::spinLock(int32_t *pLock) {
  int ret = 0;
  volatile uint64_t timeout = 0;
  /* NOTE: if the others do spinLock and then crashed, that's a diaster to the things left */
  while (__atomic_exchange_n(pLock, 1, __ATOMIC_ACQUIRE) && (0 == ret)) {
    timeout = 0;
    while (__atomic_load_n(pLock, __ATOMIC_RELAXED) && (0 == ret)) {
      timeout++;
      if (timeout > VRING_SPIN_MAX_COUNTER) {
        ASLOG(VRINGE, ("vring %s spin lock %p dead\n", m_Name.c_str(), pLock));
        return EDEADLK;
      }
    }
  }
  return ret;
}

void VRingBase::spinUnlock(int32_t *pLock) {
  __atomic_store_n(pLock, 0, __ATOMIC_RELEASE);
}

VRingWriter::VRingWriter(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc, size_t msgSize)
  : VRingBase(uioId, name, isIvshmem, numDesc, msgSize) {
}

int VRingWriter::init() {
  int ret = 0;
  std::shared_ptr<SharedMemory> sharedMemory = nullptr;

  if (m_isIvshmem) {
    sharedMemory = std::make_shared<SharedMemory>(m_uioId);
  }
  else {
    uint32_t shm_size = size();
    sharedMemory = std::make_shared<SharedMemory>(m_Name, shm_size);
  }
  if (nullptr == sharedMemory) {
    ret = ENOMEM;
  } else {
    ret = sharedMemory->create();
  }

  if (0 == ret) {
    m_SharedMemory = sharedMemory;
    m_Meta = (VRing_MetaType *)m_SharedMemory->getVA();
    m_Desc = (VRing_DescType *)(((uintptr_t)m_Meta) + VRING_SIZE_OF_META(m_numDesc));
    m_Avail = (VRing_AvailType *)(((uintptr_t)m_Desc) + VRING_SIZE_OF_DESC(m_numDesc));
    m_Used = (VRing_UsedType *)(((uintptr_t)m_Avail) + VRING_SIZE_OF_AVAIL(m_numDesc));
    ASLOG(INFO, ("Meta = %p, Desc = %p, Avail = %p, Used = %p\n", m_Meta, m_Desc, m_Avail, m_Used));
    ret = setup();
  } else {
    ASLOG(VRINGE, ("vring writer can't open shm %s\n", m_Name.c_str()));
  }

  if (0 == ret) {
    ASLOG(VRING, ("vring writer %s online: msgSize = %u,  numDesc = %u\n", m_Name.c_str(),
                  m_MsgSize, m_numDesc));
  }

  return ret;
}

VRingWriter::~VRingWriter() {
  m_Stop = true;
  if (m_Thread.joinable()) {
    m_Thread.join();
  }

  m_SharedMemory = nullptr;
}

int VRingWriter::setup() {
  uint32_t i;
  int ret = 0;

  uintptr_t base_va = (uintptr_t)m_SharedMemory->getVA();

  memset((void*)base_va, 0, size());
  m_Meta->msgSize = m_MsgSize;
  m_Meta->numDesc = m_numDesc;
  uintptr_t buffer_va = (uintptr_t)m_Used + VRING_SIZE_OF_ALL_USED(m_numDesc);
  for (i = 0; i < m_numDesc; i++) {
        m_Desc[i].len = m_MsgSize;
        m_Avail->ring[i] = i;
        m_Desc[i].buffer = buffer_va - base_va;
        buffer_va += m_MsgSize;
  }
  m_Avail->idx = m_numDesc;
  m_Avail->lastIdx = 0;

  return ret;
}

int VRingWriter::get(void *&buf, uint32_t &idx, uint32_t &len) {

  uint32_t last_idx = m_Avail->lastIdx; 
  uint32_t avail_idx = __atomic_load_n(&m_Avail->idx, __ATOMIC_ACQUIRE);

  if (last_idx == avail_idx) {
    /* no buffers */
    return ENODATA;
  }

  idx = m_Avail->ring[last_idx % m_numDesc];
  uintptr_t base_va = (uintptr_t)m_SharedMemory->getVA();
  buf = (void*)(base_va + m_Desc[idx].buffer);
  len = m_Desc[idx].len;
  m_Avail->lastIdx = last_idx + 1;
  ASLOG(VRING, ("vring writer %s: get DESC[%u], len = %u; AVAIL: lastIdx = %u, idx = %u\n",
              m_Name.c_str(), idx, len, m_Avail->lastIdx, avail_idx));

  return 0;
}

int VRingWriter::put(uint32_t idx, uint32_t len) {
  VRing_UsedType *used;
  VRing_UsedElemType *usedElem;

  if (idx >= m_numDesc) {
    return EINVAL;
  } 

  m_Desc[idx].timestamp = timestamp();

  __atomic_store_n(&m_Desc[idx].ref, 1, __ATOMIC_RELEASE);
  used = (VRing_UsedType *)m_Used;
  uint32_t cur_idx = used->idx; 
  usedElem = &used->ring[cur_idx % m_numDesc];
  usedElem->id = idx;
  usedElem->len = len;
  __atomic_store_n(&used->idx, cur_idx + 1, __ATOMIC_RELEASE);

  return 0;
}

int VRingWriter::drop(uint32_t idx) {
  if (idx >= m_numDesc) {
    return EINVAL;
  }

  uint32_t last_idx = m_Avail->lastIdx;
  if (last_idx == 0) {
    ASLOG(INFO, ("vring writer %s: drop DESC[%u] failed, lastIdx is already 0!\n", m_Name.c_str(), idx));
    return EFAULT;
  }
  if (m_Avail->ring[(last_idx - 1) % m_numDesc] != idx) {
    ASLOG(VRINGE, ("vring writer %s: drop DESC[%u] mismatch with ring buffer slot!\n", m_Name.c_str(), idx));
    return EFAULT;
  }

  m_Avail->lastIdx = last_idx - 1;
  ASLOG(VRING, ("vring writer %s: drop DESC[%u] success; AVAIL roll back: lastIdx = %u\n",
                m_Name.c_str(), idx, m_Avail->lastIdx));

  return 0;
}


VRingReader::VRingReader(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc, size_t msgSize) 
  : VRingBase(uioId, name, isIvshmem, numDesc, msgSize) {
}

int VRingReader::init() {
  uint32_t i;
  VRing_UsedType *used;
  int32_t ref;
  int ret = 0;

  std::shared_ptr<SharedMemory> sharedMemory = nullptr;

  if (m_isIvshmem) {
    sharedMemory = std::make_shared<SharedMemory>(m_uioId);
  }
  else {
    sharedMemory = std::make_shared<SharedMemory>(m_Name, size());
  }

  if (nullptr == sharedMemory) {
    ret = ENOMEM;
  } else {
    ret = sharedMemory->create();
  }

  if (0 == ret) {
    m_SharedMemory = sharedMemory;
    m_Meta = (VRing_MetaType *)m_SharedMemory->getVA();
    assert(m_Meta->numDesc == m_numDesc);
    m_Desc = (VRing_DescType *)(((uintptr_t)m_Meta) + VRING_SIZE_OF_META(m_numDesc));
    m_Avail = (VRing_AvailType *)(((uintptr_t)m_Desc) + VRING_SIZE_OF_DESC(m_numDesc));
    used = (VRing_UsedType *)(((uintptr_t)m_Avail) + VRING_SIZE_OF_AVAIL(m_numDesc));
    for (i = 0; i < VRING_MAX_READERS; i++) {
      if (VRING_USED_STATE_FREE == __atomic_load_n(&used->state, __ATOMIC_ACQUIRE)) {
        ref = __atomic_fetch_add(&used->state, 1, __ATOMIC_ACQUIRE);
        if (VRING_USED_STATE_FREE == ref) {
          m_ReaderIdx = i;
          m_Used = used;
          std::atomic_thread_fence(std::memory_order_release);  // ????
          ref = __atomic_add_fetch(&m_Used->state, 1, __ATOMIC_ACQ_REL);
          assert(VRING_USED_STATE_READY == ref);
          ASLOG(INFO, ("reader is ready!\n"));

          break;
        } else {
          ASLOG(VRING, ("vring reader %s: race on %u\n", m_Name.c_str(), i));
          __atomic_fetch_sub(&used->state, 1, __ATOMIC_ACQ_REL);
        }
      }
      used = (VRing_UsedType *)(((uintptr_t)used) + VRING_SIZE_OF_USED(m_numDesc));
    }

    if (nullptr == m_Used) {
      ASLOG(VRINGE, ("vring reader %s no free used ring\n", m_Name.c_str()));
      ret = ENOSPC;
    }
  } else {
    ASLOG(VRINGE, ("vring reader can't open shm %s\n", m_Name.c_str()));
  }

  if (0 == ret) {
    ASLOG(VRINGI, ("vring reader %s@%u online: msgSize = %u,  numDesc = %u\n", m_Name.c_str(),
                   m_ReaderIdx, m_Meta->msgSize, m_numDesc));
  }

  return ret;
}

VRingReader::~VRingReader() {
  void *addr;
  uint32_t idx = -1;
  uint32_t len = 0;
  int ret = 0;
  int32_t ref;

  m_Stop = true;
  if (m_Thread.joinable()) {
    m_Thread.join();
  }

  if (nullptr != m_Used) {
    ref = __atomic_load_n(&m_Used->state, __ATOMIC_ACQUIRE);
    if (VRING_USED_STATE_READY == ref) {
      ret = get(addr, idx, len);
      while (0 == ret) {
        (void)put(idx);
        ret = get(addr, idx, len);
        //ASLOG(VRINGI, ("vring reader %s@%u, release unconsumed buffer at %u\n", m_Name.c_str(),
                       //m_ReaderIdx, idx));
      }
      __atomic_sub_fetch(&m_Used->state, VRING_USED_STATE_READY, __ATOMIC_ACQ_REL);
      ASLOG(VRINGI, ("vring reader %s@%u clear up\n", m_Name.c_str(), m_ReaderIdx));
    } else {
      ASLOG(VRINGE, ("vring reader %s@%u killed\n", m_Name.c_str(), m_ReaderIdx));
    }
  }

  m_SharedMemory = nullptr;
}

int VRingReader::get(void *&buf, uint32_t &idx, uint32_t &len) {
  VRing_UsedElemType *used;
  int ret = 0;

  if (VRING_USED_STATE_READY != __atomic_load_n(&m_Used->state, __ATOMIC_ACQUIRE)) {
    ASLOG(VRINGE, ("vring reader %s@%u get killed by writer, state = %d\n", m_Name.c_str(), m_ReaderIdx, m_Used->state));
    return EBADF; /* killed by the Writer */
  } 

  uint32_t last_idx = m_Used->lastIdx;
  uint32_t used_idx = __atomic_load_n(&m_Used->idx, __ATOMIC_ACQUIRE);

  if (last_idx == used_idx) {
    /* no used buffer available */
    return ENOMSG;
  } 

  used = &m_Used->ring[last_idx % m_numDesc];
  idx = __atomic_load_n(&used->id, __ATOMIC_ACQUIRE);
  len = __atomic_load_n(&used->len, __ATOMIC_RELAXED);
  m_Used->lastIdx = last_idx + 1;

  uintptr_t base_va = (uintptr_t)m_SharedMemory->getVA();
  buf = (void*)(base_va + m_Desc[idx].buffer); 

  return ret;
}

int VRingReader::put(uint32_t idx) {
  if (VRING_USED_STATE_READY != __atomic_load_n(&m_Used->state, __ATOMIC_ACQUIRE)) {
    ASLOG(VRINGE, ("vring reader %s@%u put killed by writer\n", m_Name.c_str(), m_ReaderIdx));
    return EBADF; /* killed by the Writer */
  } 

  if (idx >= m_numDesc) {
    return EINVAL;
  } 

  __atomic_store_n(&m_Desc[idx].ref, 0, __ATOMIC_RELAXED);
  uint32_t cur_idx = m_Avail->idx; 
  m_Avail->ring[cur_idx % m_numDesc] = idx;

  ASLOG(VRING, ("vring reader %s@%u: put DESC[%u]; AVAIL: lastIdx = %u, idx = %u\n",
                m_Name.c_str(), m_ReaderIdx, idx, m_Avail->lastIdx, cur_idx));

  // 核心同步点：使用 RELEASE 屏障。确保 ring[] 的写入完成后，再更新指针通知 Writer 回收
  __atomic_store_n(&m_Avail->idx, cur_idx + 1, __ATOMIC_RELEASE);

  return 0;
}

} // namespace vdds
} // namespace as
