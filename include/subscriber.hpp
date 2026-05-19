/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
#ifndef _VRING_DDS_SUBSCRIBER_HPP_
#define _VRING_DDS_SUBSCRIBER_HPP_
/* ================================ [ INCLUDES  ] ============================================== */
#include "vring.hpp"
#include <string>
#include <map>
#include <mutex>

#include "Std_Debug.h"

namespace as {
namespace vdds {
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_VSUBE 3
/* ================================ [ TYPES     ] ============================================== */
typedef struct SubscriberOptions {
public:
  uint32_t queueDepth = 1;
  size_t msgSize = 0;
  std::string uioId;
  std::string name;
  bool isIvshmem = false;  // true for publisher (creates shm), false for subscriber
} SubscriberOptions_t;

template <typename T> class Subscriber {
public:
  Subscriber(const SubscriberOptions_t &options);
  ~Subscriber();

  int init();

  int receive(T *&sample, size_t &size);

  int release(T *sample);

  uint32_t idx(T *sample);

private:
  std::string m_TopicName;
  VRingReader m_Reader;
  std::mutex m_Mutex;
  std::map<T *, uint32_t> m_IdxMap;
};
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
/* ================================ [ LOCALS    ] ============================================== */
/* ================================ [ FUNCTIONS ] ============================================== */
template <typename T>
Subscriber<T>::Subscriber(const SubscriberOptions_t &options)
  :  m_Reader(options.uioId, options.name, options.isIvshmem, options.queueDepth, options.msgSize) {
}

template <typename T> Subscriber<T>::~Subscriber() {
}

template <typename T> int Subscriber<T>::init() {
  return m_Reader.init();
}

template <typename T> int Subscriber<T>::receive(T *&sample, size_t &size) {
  int ret = 0;
  uint32_t idx = -1;
  uint32_t len = 0;

  ret = m_Reader.get((void *&)sample, idx, len);
  if (0 == ret) {
    m_IdxMap[sample] = idx;
    size = len;
  }

  return ret;
}

template <typename T> int Subscriber<T>::release(T *sample) {
  int ret = 0;
  uint32_t idx;

  auto it = m_IdxMap.find(sample);
  if (it != m_IdxMap.end()) {
    idx = it->second;
    ret = m_Reader.put(idx);
    if (0 == ret) {
      m_IdxMap.erase(it);
    }
  } else {
    ASLOG(VSUBE, ("%s: invalid sample\n", m_TopicName.c_str()));
    ret = EINVAL;
  }

  return ret;
}

template <typename T> uint32_t Subscriber<T>::idx(T *sample) {
  uint32_t idx_ = -1;
  std::unique_lock<std::mutex> lck(m_Mutex);
  auto it = m_IdxMap.find(sample);
  if (it != m_IdxMap.end()) {
    idx_ = it->second;
  } else {
    ASLOG(VSUBE, ("%s: invalid sample\n", m_TopicName.c_str()));
  }

  return idx_;
}

} // namespace vdds
} // namespace as
#endif /* _VRING_DDS_SUBSCRIBER_HPP_ */
