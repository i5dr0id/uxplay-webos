#pragma once
#include <cstdint>
#include <cstring>
#include <unistd.h>

#define BRIDGE_SOCK "/tmp/uxplay-av.sock"

enum : uint8_t { MSG_PACKET=1, MSG_STATE=2, MSG_START=3, MSG_END=4, MSG_PIN=5 };

struct BridgeHeader {
    uint8_t  type;
    uint8_t  flags;
    uint16_t codec;
    uint32_t size;
    uint64_t timestamp;
};

static inline bool bridge_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    while (len > 0) { ssize_t n = write(fd, p, len); if (n <= 0) return false; p += n; len -= n; }
    return true;
}
static inline bool bridge_read_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t*)buf;
    while (len > 0) { ssize_t n = read(fd, p, len); if (n <= 0) return false; p += n; len -= n; }
    return true;
}
