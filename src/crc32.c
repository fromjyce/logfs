#include "crc32.h"
#include <pthread.h>

/* Table build is lazy but must still be race-free: txn.c's commit thread
 * and the caller's main thread can both reach the first checksum before
 * either has observed the other's write, so a plain "ready" flag would be
 * a data race. pthread_once makes the one-time build safe under that. */
static uint32_t crc32_table[256];
static pthread_once_t crc32_table_once = PTHREAD_ONCE_INIT;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
}

uint32_t logfs_crc32_init(void) {
    pthread_once(&crc32_table_once, crc32_build_table);
    return 0xFFFFFFFFu;
}

uint32_t logfs_crc32_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t logfs_crc32_final(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}
