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
} HelloWorld_t;
/* ================================ [ DECLARES  ] ============================================== */
/* ================================ [ DATAS     ] ============================================== */
static bool lStopped = false;
/* ================================ [ LOCALS    ] ============================================== */
static void signalHandler(int sig) {
  lStopped = true;
}
/* ================================ [ FUNCTIONS ] ============================================== */
int main(int argc, char *argv[]) {
  int r = 0;
  uint32_t sessionId = 0, size = 0, len = 0;
  uint64_t sum = 0;
  int periodMs = 1000;
  char *filePath;
  FILE *fileFd;

  PublisherOptions_t options;

  int opt;
  while ((opt = getopt(argc, argv, "f:l:n:p:q:u:")) != -1) {
    switch (opt) {
      case 'f':
        filePath = strdup(optarg);
        if (access(filePath, F_OK) == 0) {
          fileFd = fopen(filePath, "rb");
          if (fileFd == NULL) {
            ASLOG(ERROR, ("open file error: %s \n", filePath));
            return -1;
          }
        } else {
          ASLOG(INFO, ("open file error\n"));
          return -1;
        }
        ASLOG(INFO, ("open file: %s \n", filePath));
        break;
      case 'l':
        size = atoi(optarg);
        if (size > MAX_LEN) {
          ASLOG(INFO, ("size not > %u\n", MAX_LEN));
        }
        ASLOG(INFO, ("size: %u\n", size));
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
      break;
        default:
        break;
    }
  }

  signal(SIGINT, signalHandler);

  Publisher<HelloWorld_t> pub(options);
  r = pub.init();
  while ((0 == r) && (false == lStopped)) {
    HelloWorld_t *sample = nullptr;
    r = pub.load(sample);
    if (0 == r) {
      len = fread(sample->string, 1, size, fileFd);
      if (len == 0) {
        fclose(fileFd);
        ASLOG(INFO, ("close: %s, Write: %lu byte, senssion = %lu\n", filePath, sum, sessionId));
        return 0;
      }
      //ASLOG(INFO, ("publish: %s, idx = %u\n", sample->string, pub.idx(sample)));
      r = pub.publish(sample, len);
      sum += len;
      sessionId++;
    } else if ((ETIMEDOUT == r) || (ENODATA == r)) {
      r = 0;
    } else {
      ASLOG(ERROR, ("exit as error %d\n", r));
    }
    usleep(10);
  }

  return r;
}
