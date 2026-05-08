/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include <sys/mman.h>
#include <fcntl.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <fstream>

#include "shared_memory.hpp"
#include "Std_Debug.h"

namespace as {
namespace vdds {
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_MEMORY 0
#define AS_LOG_MEMORYI 2
#define AS_LOG_MEMORYE 3
/* ================================ [ TYPES     ] ============================================== */
SharedMemory::SharedMemory(std::string uioId)
  : uio_DeviceFile("/dev/uio" + uioId), 
    uio_ResourceFile("/sys/class/uio/uio" + uioId + "/device/resource"), 
    uio_Resource2wcFile("/sys/class/uio/uio" + uioId + "/device/resource2_wc"), 
    uio_ConfigFile("/sys/class/uio/uio" + uioId + "/device/config"),
    uio_doorbellFile("/sys/class/uio/uio" + uioId + "/device/resource0"),
    m_Type(MEMORY_TYPE_IVSHMEM) {
}

SharedMemory::SharedMemory(std::string name, uint32_t size)
  : m_Name("/" + name), m_Size(size), m_Type(MEMORY_TYPE_ALLOC) {
}

int SharedMemory::create() {
  int ret = 0;
  int fd = -1;
  void *addr = nullptr;
  int oflag = O_RDWR;


  if (m_Type == MEMORY_TYPE_IVSHMEM) {
    fd = open(uio_Resource2wcFile.c_str(), oflag);
    if (fd < 0) {
      ASLOG(MEMORYE, ("ivshmem open failed: %s\n", uio_Resource2wcFile.c_str()));
      return ENOENT;
    }
    m_Size = getSize();
  } else {
    fd = shm_open(m_Name.c_str(), oflag, 0600);
    if (fd < 0) {
      ASLOG(MEMORYE, ("shm %s open failed\n", m_Name.c_str()));
      return EEXIST;
    }
    ret = ftruncate(fd, m_Size);
    if (ret != 0) {
      ASLOG(MEMORYE, ("shm %s ftruncate failed\n", m_Name.c_str()));
      close(fd);
      return ENOMEM;
    }
  }

  addr = mmap(NULL, m_Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (nullptr != addr) {
    m_Addr = addr;
  } else {
    ASLOG(MEMORYE, ("mmap failed, shmem: %s\n", (m_Type == MEMORY_TYPE_IVSHMEM) ? uio_Resource2wcFile.c_str() : m_Name.c_str()));
    close(fd);
    ret = ENOMEM;
  }

  ASLOG(INFO, ("open success, shm: %s, size: %d\n", (m_Type == MEMORY_TYPE_IVSHMEM) ? uio_Resource2wcFile.c_str() : m_Name.c_str(), m_Size));

  return ret;
}

int SharedMemory::release() {
  int ret = 0;

  if (nullptr != m_Addr) {
    ret |= munmap(m_Addr, m_Size);
  }

#if defined(linux)
  if (m_Handle > 0) {
    ret |= close(m_Handle);
  }
#endif

  if (MEMORY_TYPE_ALLOC == m_Type) {
    if (m_Handle > 0) {
      ret |= shm_unlink(m_Name.c_str());
    }
  }

  m_Addr = nullptr;
  m_Handle = 0;

  if (0 != ret) {
    ASLOG(MEMORYE, ("memory %s release failed %d\n", m_Name.c_str(), ret));
  }

  return ret;
}

SharedMemory::~SharedMemory() {
  release();
}

uint32_t SharedMemory::getSize() {
  unsigned long long start, end, flags;

  if (m_Type == MEMORY_TYPE_IVSHMEM) {
    unsigned long long start = 0, end = 0, flags = 0;

    if (m_Type == MEMORY_TYPE_IVSHMEM) {
      std::ifstream resFile(uio_ResourceFile);

      if (!resFile.is_open()) {
        return 0;
      }

      std::string line;
      for (int i = 0; i < 3; ++i) {
        if (!std::getline(resFile, line)) {
          return 0;
        }
      }

      if (sscanf(line.c_str(), "%llx %llx %llx", &start, &end, &flags) == 3) {
        if (start == 0 && end == 0) {
          return 0;
        }
        m_Size = (uint32_t)(end - start + 1);
        return m_Size;
      }
    }
    return m_Size;
  } else {
    return m_Size;
  }
}

/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
/* ================================ [ LOCALS    ] ============================================== */
/* ================================ [ FUNCTIONS ] ============================================== */
} // namespace vdds
} // namespace as
