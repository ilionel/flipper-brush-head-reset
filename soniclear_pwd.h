#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * Compute the NTAG213 write password of a smart-toothbrush brush head.
 *
 * Algorithm (reverse-engineered by @ATC1441, verified against real heads):
 * CRC16-CCITT (poly 0x1021) with init 0x49A3 over the 7-byte UID,
 * then a second CRC16 over the MFG code seeded with the first result; the two 16-bit
 * results form a 32-bit word that is byte-swapped within each half. The password is
 * handle-independent: it depends only on the head's UID + MFG.
 *
 * @param uid      7-byte tag UID
 * @param mfg      MFG code bytes (pages 0x21-0x23, offset 2, 10 ASCII chars e.g. "010203 99Z")
 * @param mfg_len  length of mfg (normally 10)
 * @param pwd_out  receives the 4-byte password (PWD_AUTH key), MSB first
 */
void soniclear_pwd_compute(
    const uint8_t uid[7],
    const uint8_t* mfg,
    size_t mfg_len,
    uint8_t pwd_out[4]);
