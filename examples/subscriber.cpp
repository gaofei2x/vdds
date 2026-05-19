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
#define MAX_LEN (50 * 1024 * 1024)
typedef struct {
  char string[MAX_LEN];
} ShmMessage_t;
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

double Torate(double byte, double usec)
{
  if (usec <= 0.0) return 0.0; // 防御性代码，防止依旧除零
  double sec = usec / (1000.0 * 1000.0);
  return byte / sec / (1024.0 * 1024.0);
}

void print_help(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -f <freq>       Set TSC clock frequency (in MHz or Hz)\n");
    printf("  -l <size>       Set message size (Max: %u, must match publisher/subscriber)\n", MAX_LEN);
    printf("  -u <id>         Set UIO ID (Enables Ivshmem mode, e.g., uio<id>)\n");
    printf("  -q <depth>      Set queue depth (Must match publisher/subscriber)\n");
    printf("  -n <name>       Set ivshmem name\n");
    printf("  -h              Show this help message and exit\n");
    printf("Examples:\n");
    printf("sudo ./VDDSSub -f 3686 -l 10240 -q 100 -n ivshmem0 \n");
    printf("sudo ./VDDSSub -f 3686 -l 10240 -q 100 -u 0 \n");
}
/* ================================ [ FUNCTIONS ] ============================================== */
int main(int argc, char *argv[]) {
  int r = 0, opt = 0;
  uint32_t freq = 0;
  uint64_t tscDeltaTotal = 0, times = 0;
  uint64_t tscPrev = 0, tscCurr = 0, tscStart = 0, tscEnd = 0;
  SubscriberOptions_t options;
  size_t size = 0, receiveSize = 0;

  while ((opt = getopt(argc, argv, "f:l:n:q:u:h:")) != -1) {
    switch (opt) {
      case 'f':
        freq = atoi(optarg);
        ASLOG(INFO, ("freq: %d\n", freq));
        break;
      case 'l':
        options.msgSize = atoi(optarg);
        if (options.msgSize > MAX_LEN) {
          ASLOG(INFO, ("msgSize not > %u\n", MAX_LEN));
        }
        ASLOG(INFO, ("msgSize: %u\n", options.msgSize));
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
      case 'h': 
        print_help(argv[0]);
        return 0;
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  signal(SIGINT, signalHandler);

  Subscriber<ShmMessage_t> sub(options);
  r = sub.init();
  while ((0 == r) && (false == lStopped)) {
    ShmMessage_t *sample = nullptr;
    r = sub.receive(sample, size);
    if (0 == r) {
      receiveSize += size;
      tscCurr = get_tsc();
      if (times == 0) {
        tscStart = tscCurr;
      }
      if (tscPrev != 0) {
        tscDeltaTotal += (tscCurr - tscPrev);
      }
      tscPrev = tscCurr;
      times++;
      sub.release(sample);
    } else if ((ETIMEDOUT == r) || (ENOMSG == r)) {
      std::this_thread::yield(); 
      r = 0;
    } else {
      ASLOG(ERROR, ("exit as error %d\n", r));
    }
  }

   tscEnd = tscCurr; 

  if (times > 1 && tscDeltaTotal > 0 && tscEnd > tscStart) {
    double avg_cycles = (double)tscDeltaTotal / (times - 1); 
    
    double real_freq_hz = (double)freq;
    if (real_freq_hz < 10000000.0) { 
        real_freq_hz = real_freq_hz * 1000000.0; 
    }

    uint64_t total_elapsed_cycles = tscEnd - tscStart;
    double total_duration_sec = (double)total_elapsed_cycles / real_freq_hz;
    
    double latency_usec = (avg_cycles / real_freq_hz) * 1000000.0;

    double real_data_rate = 0.0;
    if (total_duration_sec > 0.0) {
        real_data_rate = ((double)receiveSize / (1024.0 * 1024.0)) / total_duration_sec;
    }

    ASLOG(INFO, ("[ receive times: %u, tsc total elapsed = %lu, receive Byte: %lu ]\n", 
                    times, total_elapsed_cycles, receiveSize)); 
    ASLOG(INFO, ("[ tsc delta avg = %.2f ]\n", avg_cycles));
    ASLOG(INFO, ("[ latency(usec) = %.2f us ]\n", latency_usec)); 
    ASLOG(INFO, ("[ data rate: %.2f MB/s ]\n", real_data_rate));
  }

  return 0;
}
