#pragma once
#include <stddef.h>
#include <stdint.h>

/* Shared reflected CRC for the legacy B and native C bounded transports. */
static inline uint32_t pocket_wire_crc(const uint8_t *p, size_t n) {
    uint32_t c=0xffffffffu;
    while(n--) { c^=*p++; for(unsigned i=0;i<8;i++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return c^0xffffffffu;
}
static inline uint32_t pocket_wire_read32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline void pocket_wire_write32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
