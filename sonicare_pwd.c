#include "sonicare_pwd.h"

static uint16_t sonicare_crc16_ccitt(uint16_t crc, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for(int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void sonicare_pwd_compute(
    const uint8_t uid[7],
    const uint8_t* mfg,
    size_t mfg_len,
    uint8_t pwd_out[4]) {
    uint16_t lo = sonicare_crc16_ccitt(0x49A3, uid, 7);
    uint16_t hi = sonicare_crc16_ccitt(lo, mfg, mfg_len);
    uint32_t c = ((uint32_t)hi << 16) | lo;
    // byte-swap within each 16-bit half
    c = ((c >> 8) & 0x00FF00FFu) | ((c << 8) & 0xFF00FF00u);
    pwd_out[0] = (c >> 24) & 0xFF;
    pwd_out[1] = (c >> 16) & 0xFF;
    pwd_out[2] = (c >> 8) & 0xFF;
    pwd_out[3] = c & 0xFF;
}
