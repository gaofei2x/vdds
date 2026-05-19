/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2024 Parai Wang <parai@foxmail.com>
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include "vdds.hpp"
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

void print_help(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -l <size>       Set message size (Max: %u, must match publisher/subscriber)\n", MAX_LEN);
    printf("  -u <id>         Set UIO ID (Enables Ivshmem mode, e.g., uio<id>)\n");
    printf("  -q <depth>      Set queue depth (Must match publisher/subscriber)\n");
    printf("  -n <name>       Set ivshmem name\n");
    printf("  -h              Show this help message and exit\n");
    printf("Examples:\n");
    printf("sudo ./VDDSPub -l 10240 -q 100 -n ivshmem0 \n");
    printf("sudo ./VDDSPub -l 10240 -q 100 -u 0 \n");
}

/* ================================ [ FUNCTIONS ] ============================================== */
int main(int argc, char *argv[]) {
  int r = 0;
  uint32_t sessionId = 0, size = 0, len = 0;
  uint64_t sum = 0;
  int periodMs = 1000;
  char *filePath = nullptr;
  FILE *fileFd = nullptr;

  PublisherOptions_t options;
  size_t dictOffset = 0;
  size_t totalSimulatedRead = 0;
  size_t targetTestSize = 1000UL * 1024 * 1024 * 1024; 

  options.msgSize = 1024;

  int opt;
  while ((opt = getopt(argc, argv, "h:l:n:p:q:u:")) != -1) {
    switch (opt) {
      case 'l':
        options.msgSize = atoi(optarg);
        if (options.msgSize > MAX_LEN) {
          ASLOG(INFO, ("msgSize not > %u\n", MAX_LEN));
          return -1;
        }
        ASLOG(INFO, ("msgSize: %u\n", options.msgSize));
        break;
      case 'u':
        options.uioId = optarg;
        options.isIvshmem = true;
        ASLOG(INFO, ("uioN: uio%s\n", options.uioId));
        break;
      case 'p':
        periodMs = atoi(optarg);
        ASLOG(INFO, ("periodMS: %dms\n", periodMs));
        break;
      case 'q':
        options.queueDepth = atoi(optarg);
        ASLOG(INFO, ("queueDepth: %d\n", options.queueDepth));
        break;
      case 'n':
        options.name = optarg;
        ASLOG(INFO, ("name: %s\n", options.name.c_str()));
        break;
      case 'h': 
        print_help(argv[0]);
        return 0;
      default:
        print_help(argv[0]);
        return -1;
    }
  }

  size_t msgSize = options.msgSize; 

  size_t dictSize = 4 * 1024 * 1024; 
  char* randomDict = (char*)std::malloc(dictSize);

  for (size_t i = 0; i < dictSize; i++) {
      randomDict[i] = (char)((i * 33 + 13) % 256); 
  }

  signal(SIGINT, signalHandler);

  Publisher<ShmMessage_t> pub(options);
  r = pub.init();
  while ((0 == r) && (false == lStopped)) {
    if (totalSimulatedRead >= targetTestSize) {
        ASLOG(INFO, ("Simulated 10GB memory run complete. Total Sent: %lu byte\n", totalSimulatedRead));
        break;
    }

    ShmMessage_t *sample = nullptr;
    r = pub.load(sample);
    if (0 == r) {
      std::memcpy(sample->string, randomDict + dictOffset, msgSize);
      r = pub.publish(sample, msgSize);
      if (0 == r) {
        totalSimulatedRead += msgSize;
        sessionId++;

        dictOffset += msgSize;
        if (dictOffset + msgSize > dictSize) {
            dictOffset = 0; 
        }
      } else {
        ASLOG(ERROR, ("publish failed with error %d, dropping sample!\n", r));
        pub.drop(sample); 
        r = 0;
      }
    } else if ((ETIMEDOUT == r) || (ENODATA == r)) {
      r = 0;
    } else {
      ASLOG(ERROR, ("exit as error %d\n", r));
    }
    //usleep(100000);
  }

  return r;
}
