#pragma once

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_packet {
    struct timespec timestamp;
    char ingress_if[32];
    uint8_t* data;
    size_t caplen;
    size_t len;
} fh_packet_t;

#ifdef __cplusplus
}
#endif
