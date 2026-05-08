/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
#ifndef _VRING_DDS_PUBLISHER_HPP_
#define _VRING_DDS_PUBLISHER_HPP_
/* ================================ [ INCLUDES  ] ============================================== */
#include "vring.hpp"
#include <string>
#include <map>
#include <mutex>

#include "Std_Debug.h"

namespace as {
namespace vdds {
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_VPUBE 3
/* ================================ [ TYPES     ] ============================================== */
typedef struct PublisherOptions {
public:
  uint32_t queueDepth = 1;
  std::string uioId;
  std::string name;
  bool isIvshmem = false;  // true for publisher (creates shm), false for subscriber
} PublisherOptions_t;

template <typename T> class Publisher {
public:
  Publisher(const PublisherOptions_t &options);
  ~Publisher();

  int init();

  int load(T *&sample);
  int publish(T *sample);
  int publish(T *sample, size_t size);

  // API for debug purpose
  uint32_t idx(T *sample);

private:
  std::string m_TopicName;
  VRingWriter m_Writer;
  std::mutex m_Mutex;
  std::map<T *, uint32_t> m_IdxMap;
};
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
/* ================================ [ LOCALS    ] ============================================== */
/* ================================ [ FUNCTIONS ] ============================================== */
template <typename T>
Publisher<T>::Publisher(const PublisherOptions_t &options)
  : m_Writer(options.uioId, options.name, options.isIvshmem, options.queueDepth, sizeof(T)) {
}

template <typename T> Publisher<T>::~Publisher() {
}

template <typename T> int Publisher<T>::init() {
  return m_Writer.init();
}

template <typename T> int Publisher<T>::load(T *&sample) {
  uint32_t idx;
  uint32_t len;
  int ret = 0;

  ret = m_Writer.get((void *&)sample, idx, len);
  if (0 == ret) {
    std::unique_lock<std::mutex> lck(m_Mutex);
    m_IdxMap[sample] = idx;
  }

  return ret;
}

template <typename T> int Publisher<T>::publish(T *sample) {
  int ret = 0;
  uint32_t idx;

  std::unique_lock<std::mutex> lck(m_Mutex);
  auto it = m_IdxMap.find(sample);
  if (it != m_IdxMap.end()) {
    idx = it->second;
    ret = m_Writer.put(idx, sizeof(T));
  } else {
    ASLOG(VPUBE, ("%s: invalid sample\n", m_TopicName.c_str()));
    ret = EINVAL;
  }

  return ret;
}

template <typename T> int Publisher<T>::publish(T *sample, size_t size) {
  int ret = 0;
  uint32_t idx;

  std::unique_lock<std::mutex> lck(m_Mutex);
  auto it = m_IdxMap.find(sample);
  if (it != m_IdxMap.end()) {
    idx = it->second;
    m_IdxMap.erase(it);
    m_Writer.put(idx, (uint32_t)size);
  } else {
    ASLOG(VPUBE, ("%s: invalid sample\n", m_TopicName.c_str()));
    ret = EINVAL;
  }

  return ret;
}

template <typename T> uint32_t Publisher<T>::idx(T *sample) {
  uint32_t idx_ = -1;
  std::unique_lock<std::mutex> lck(m_Mutex);
  auto it = m_IdxMap.find(sample);
  if (it != m_IdxMap.end()) {
    idx_ = it->second;
  } else {
    ASLOG(VPUBE, ("%s: invalid sample\n", m_TopicName.c_str()));
  }

  return idx_;
}

} // namespace vdds
} // namespace as
#endif /* _VRING_DDS_PUBLISHER_HPP_ */
