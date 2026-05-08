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
VRingBase::VRingBase(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc)
  : m_uioId(uioId), m_Name(name), m_isIvshmem(isIvshmem), m_numDesc(numDesc) {
}

VRingBase::~VRingBase() {
}

uint32_t VRingBase::size() {
  return VRING_SIZE_OF_META(m_numDesc) + VRING_SIZE_OF_DESC(m_numDesc) +
         VRING_SIZE_OF_AVAIL(m_numDesc) + VRING_SIZE_OF_ALL_USED(m_numDesc);
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
        ret = EDEADLK;
        ASLOG(VRINGE, ("vring %s spin lock %p dead\n", m_Name.c_str(), pLock));
      }
    }
  }
  return ret;
}

void VRingBase::spinUnlock(int32_t *pLock) {
  __atomic_store_n(pLock, 0, __ATOMIC_RELEASE);
}

VRingWriter::VRingWriter(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc, uint32_t msgSize)
  : VRingBase(uioId, name, isIvshmem, numDesc), m_MsgSize(msgSize) {
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

  memset(m_SharedMemory->getVA(), 0, size());
  m_Meta->msgSize = m_MsgSize;
  m_Meta->numDesc = m_numDesc;
  for (i = 0; i < m_numDesc; i++) {
        m_Desc[i].len = m_MsgSize;
        m_Avail->ring[i] = i;
        m_Avail->idx++;
  }

  return ret;
}

int VRingWriter::get(void *&buf, uint32_t &idx, uint32_t &len) {
  int ret = 0;
  int32_t ref;

  ret = spinLock(&m_Avail->spin);
  if (0 == ret) {
    if (m_Avail->lastIdx == m_Avail->idx) {
      /* no buffers */
      ret = ENODATA;
    } else {
      idx = m_Avail->ring[m_Avail->lastIdx % m_numDesc];
      ref = __atomic_load_n(&m_Desc[idx].ref, __ATOMIC_ACQUIRE);
      if (0 == ref) {
        buf = m_Desc[idx].buffer;
        len = m_Desc[idx].len;
        m_Avail->lastIdx++;
        ASLOG(VRING, ("vring writer %s: get DESC[%u], len = %u; AVAIL: lastIdx = %u, idx = %u\n",
                      m_Name.c_str(), idx, len, m_Avail->lastIdx, m_Avail->idx));
      } else {
        ASLOG(VRINGE, ("vring writer %s: get DESC[%u] with ref = %d\n", m_Name.c_str(), idx, ref));
        ret = EBADF;
      }
    }
    spinUnlock(&m_Avail->spin);
  } else {
    ASLOG(VRINGE, ("vring writer %s: get lock AVAIL spin timeout\n", m_Name.c_str()));
  }

  return ret;
}

int VRingWriter::put(uint32_t idx, uint32_t len) {
  VRing_UsedType *used;
  VRing_UsedElemType *usedElem;
  uint32_t i;
  bool isUSed = false;
  int ret = 0;

  if (idx > m_numDesc) {
    ret = EINVAL;
  } else {
    ret = spinLock(&m_Desc[idx].spin);
    if (0 == ret) {
      m_Desc[idx].timestamp = timestamp();
      for (i = 0; i < VRING_MAX_READERS; i++) {
        used = (VRing_UsedType *)(((uintptr_t)m_Used) + VRING_SIZE_OF_USED(m_numDesc) * i);
         //if (VRING_USED_STATE_READY == __atomic_load_n(&used->state, __ATOMIC_ACQUIRE)) {
          usedElem = &used->ring[used->idx % m_numDesc];
          usedElem->id = idx;
          usedElem->len = len;
          __atomic_fetch_add(&m_Desc[idx].ref, 1, __ATOMIC_ACQ_REL);
          isUSed = true;
          ASLOG(VRING,
                ("vring writer %s@%u: put DESC[%u], len = %u ref = %d; used: lastIdx = "
                 "%u, idx = %u\n",
                 m_Name.c_str(), i, idx, len, __atomic_load_n(&m_Desc[idx].ref, __ATOMIC_ACQUIRE),
                 used->lastIdx, used->idx));
          used->idx++;
        //}
      }
      spinUnlock(&m_Desc[idx].spin);
    } else {
      ASLOG(VRINGE, ("vring writer %s: put lock DESC[%u] spin timeout\n", m_Name.c_str(), idx));
    }

    if (false == isUSed) {
      /* OK, put it back */
      (void)drop(idx);
      ret = ENOLINK;
    } 
  }

  return ret;
}

int VRingWriter::drop(uint32_t idx) {
  int ret = 0;

  if (idx > m_numDesc) {
    ret = EINVAL;
  } else {
    ASLOG(INFO, ("drop===================m_avail === %p, idx = %d, lastidx = %d\n", &m_Avail, m_Avail->idx, m_Avail->lastIdx));
    ret = spinLock(&m_Avail->spin);
    if (0 == ret) {
      m_Avail->ring[m_Avail->idx % m_numDesc] = idx;
      m_Avail->idx++;
      spinUnlock(&m_Avail->spin);
      ASLOG(VRING, ("vring writer %s: drop DESC[%u]; AVAIL: lastIdx = %u, idx = %u\n",
                    m_Name.c_str(), idx, m_Avail->lastIdx, m_Avail->idx));
    } else {
      ASLOG(VRINGE, ("vring writer %s: drop lock AVAIL spin timeout\n", m_Name.c_str()));
    }
  }

  return ret;
}

void VRingWriter::releaseDesc(uint32_t idx) {
  int32_t ref;
  int ret = 0;
  ref = __atomic_sub_fetch(&m_Desc[idx].ref, 1, __ATOMIC_ACQ_REL);
  if (0 < ref) {
    /* still used by others */
  } else if (0 == ref) {
    ret = spinLock(&m_Avail->spin);
    if (0 == ret) {
      m_Avail->ring[m_Avail->idx % m_numDesc] = idx;
      m_Avail->idx++;
      spinUnlock(&m_Avail->spin);
      ASLOG(VRINGE, ("vring writer %s: release DESC[%u]\n", m_Name.c_str(), idx));
    } else {
      ASLOG(VRINGE, ("vring writer %s: release lock AVAIL spin timeout\n", m_Name.c_str()));
    }
  } else {
    ASLOG(VRING, ("vring writer %s: release DESC[%u] ref = %d failed\n", m_Name.c_str(), idx, ref));
    assert(0);
  }
}

VRingReader::VRingReader(std::string uioId, std::string name, bool isIvshmem, uint32_t numDesc) 
  : VRingBase(uioId, name, isIvshmem, numDesc) {
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
        ASLOG(VRINGI, ("vring reader %s@%u, release unconsumed buffer at %u\n", m_Name.c_str(),
                       m_ReaderIdx, idx));
      }
      __atomic_sub_fetch(&m_Used->state, VRING_USED_STATE_READY, __ATOMIC_ACQ_REL);
        ASLOG(INFO, ("sub ====== 3\n"));
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
    ret = EBADF; /* killed by the Writer */
  } else if (m_Used->lastIdx == m_Used->idx) {
    /* no used buffer available */
    ret = ENOMSG;
  } else {
    used = &m_Used->ring[m_Used->lastIdx % m_numDesc];
    idx = used->id;
    len = used->len;
    m_Used->lastIdx++;

    buf = m_Desc[idx].buffer;

    /* if the app crashed after this before call the put, then the desc is in detached state
     * that need the monitor to recycle it.
     */
  }

  return ret;
}

int VRingReader::put(uint32_t idx) {
  int32_t ref;
  int ret = 0;

  if (VRING_USED_STATE_READY != __atomic_load_n(&m_Used->state, __ATOMIC_ACQUIRE)) {
    ret = EBADF; /* killed by the Writer */
    ASLOG(VRINGE, ("vring reader %s@%u put killed by writer\n", m_Name.c_str(), m_ReaderIdx));
  } else if (idx > m_numDesc) {
    ret = EINVAL;
  } else {
    ret = spinLock(&m_Desc[idx].spin);
    if (0 == ret) {
      ref = __atomic_sub_fetch(&m_Desc[idx].ref, 1, __ATOMIC_ACQ_REL);
      if (0 < ref) {
        /* still used by others */
      } else if (0 == ref) {
        ret = spinLock(&m_Avail->spin);
        if (0 == ret) {
          m_Avail->ring[m_Avail->idx % m_numDesc] = idx;
          ASLOG(VRING, ("vring reader %s@%u: put DESC[%u]; AVAIL: lastIdx = %u, idx = %u\n",
                        m_Name.c_str(), m_ReaderIdx, idx, m_Avail->lastIdx, m_Avail->idx));
          m_Avail->idx++;
          spinUnlock(&m_Avail->spin);
        } else {
          ASLOG(VRINGE, ("vring reader %s: put lock AVAIL spin timeout\n", m_Name.c_str(), idx));
        }
      } else {
        ASLOG(VRINGE, ("vring reader %s@%u: put DESC[%u], ref = %d\n", m_Name.c_str(), m_ReaderIdx,
                       idx, ref));
        assert(0);
        ret = EFAULT;
      }
      spinUnlock(&m_Desc[idx].spin);
    } else {
      ASLOG(VRINGE, ("vring reader %s: put lock DESC[%u] spin timeout\n", m_Name.c_str(), idx));
    }

  }

  return ret;
}

} // namespace vdds
} // namespace as
