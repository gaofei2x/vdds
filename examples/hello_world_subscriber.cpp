/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include "vdds.hpp"
#include "vring.hpp"
#include <unistd.h>
#include <signal.h>

#include "Std_Debug.h"

using namespace as::vdds;
/* ================================ [ MACROS    ] ============================================== */
/* ================================ [ TYPES     ] ============================================== */
typedef struct {
  char string[50 * 1024 * 1024];
} HelloWorld_t;
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
static bool lStopped = false;
/* ================================ [ LOCALS    ] ============================================== */
static void signalHandler(int sig) {
  lStopped = true;
}

uint64_t get_tsc(void)
{
	uint32_t lo, hi;
	asm volatile("rdtsc" : "=a" (lo), "=d" (hi));
	return ((uint64_t)hi << 32U) | lo;
} 

double Torate(uint64_t byte, uint64_t usec)
{
  double sec = 0;

  sec = (double)usec / (1000 * 1000);
  return byte / sec / (1024 * 1024);
}

/* ================================ [ FUNCTIONS ] ============================================== */
int main(int argc, char *argv[]) {
  int r = 0, opt = 0;
  uint32_t times, freq;
  uint64_t tscDeltaTotal = 0;
  uint64_t receiveSize = 0;
  SubscriberOptions_t options;
  uint64_t tscPrev = 0, tscCurr = 0;

  while ((opt = getopt(argc, argv, "f:n:q:u:")) != -1) {
    switch (opt) {
      case 'f':
        freq = atoi(optarg);
        ASLOG(INFO, ("freq: %d\n", freq));
        break;
      case 'n':
        options.name = optarg;
        ASLOG(INFO, ("name: %s\n", options.name.c_str()));
        break;
      case 'q':
        options.queueDepth = atoi(optarg);
        ASLOG(INFO, ("queueDepth: %d\n", options.queueDepth));
        break;
      case 'u':
        options.uioId = optarg;
        options.isIvshmem = true;
        ASLOG(INFO, ("uioN: uio%s\n", options.uioId.c_str()));
        break;
      break;
        default:
        break;
    }
  }

  signal(SIGINT, signalHandler);

  Subscriber<HelloWorld_t> sub(options);
  r = sub.init();
  while ((0 == r) && (false == lStopped)) {
    size_t size = 0;
    HelloWorld_t *sample = nullptr;
    r = sub.receive(sample, size);
    receiveSize += size;
    if (0 == r) {
      tscCurr = get_tsc();
      if (tscPrev != 0) {
        tscDeltaTotal += (tscCurr - tscPrev);
      }
      tscPrev = tscCurr;
      times++;
      ASLOG(INFO, ("publish: %s, idx = %u\n", sample->string, sub.idx(sample)));
      r = sub.release(sample);
    } else if ((ETIMEDOUT == r) || (ENOMSG == r)) {
      r = 0;
    } else {
      ASLOG(ERROR, ("exit as error %d\n", r));
    }
  }
  uint64_t latency = tscDeltaTotal /times / freq;
  ASLOG(INFO, ("[ receive times: %u, tsc delta total = %lu, receive Byte: %lu ]\n", 
                  times, tscDeltaTotal, receiveSize)); 
  ASLOG(INFO, ("[ tsc delta avg = %lu]\n", tscDeltaTotal/times));
  ASLOG(INFO, ("[ latency(usec) = %lu]\n", latency));
  ASLOG(INFO, ("[ data rate: %.2f MB/s ]\n", Torate(receiveSize/times, latency)));

  return 0;
}
