/*
 * can_decode.c — Pure, stateless CAN bit-field extraction.
 *
 * No LVGL, FreeRTOS, or ESP-IDF dependencies.
 */
#include "can_decode.h"

int64_t can_extract_bits(const uint8_t *data, uint8_t bit_offset,
                         uint8_t bit_length, int endian, bool is_signed)
{
    if (bit_length == 0)  return 0;
    if (bit_length > 64)  bit_length = 64;

    uint64_t value = 0;

    if (endian == 0) { /* Big-endian (Motorola) */
        uint8_t start_byte   = bit_offset / 8;
        uint8_t end_byte     = (uint8_t)((bit_offset + bit_length - 1) / 8);
        uint8_t bytes_needed = end_byte - start_byte + 1;

        if (bytes_needed == 1) {
            uint8_t bit_pos = bit_offset % 8;
            uint8_t mask    = ((1U << bit_length) - 1) << (8 - bit_pos - bit_length);
            value = (data[start_byte] & mask) >> (8 - bit_pos - bit_length);
        } else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
            for (int i = 0; i < bytes_needed && i < 8; i++)
                value = (value << 8) | data[start_byte + i];
        } else {
            uint64_t word = 0;
            for (int i = 0; i < bytes_needed && i < 8; i++)
                word = (word << 8) | data[start_byte + i];
            uint8_t shift = (uint8_t)((bytes_needed * 8) - (bit_offset % 8) - bit_length);
            value = (word >> shift) & ((1ULL << bit_length) - 1);
        }
    } else { /* Little-endian (Intel) */
        uint8_t start_byte   = bit_offset / 8;
        uint8_t end_byte     = (uint8_t)((bit_offset + bit_length - 1) / 8);
        uint8_t bytes_needed = end_byte - start_byte + 1;

        if (bytes_needed == 1) {
            uint8_t bit_pos = bit_offset % 8;
            uint8_t mask    = (1U << bit_length) - 1;
            value = (data[start_byte] >> bit_pos) & mask;
        } else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
            for (int i = 0; i < bytes_needed && i < 8; i++)
                value |= ((uint64_t)data[start_byte + i]) << (i * 8);
        } else {
            uint64_t word = 0;
            for (int i = 0; i < bytes_needed && i < 8; i++)
                word |= ((uint64_t)data[start_byte + i]) << (i * 8);
            value = (word >> (bit_offset % 8)) & ((1ULL << bit_length) - 1);
        }
    }

    /* Sign-extend */
    if (is_signed && (value & ((uint64_t)1 << (bit_length - 1))))
        value |= ~((1ULL << bit_length) - 1);

    return (int64_t)value;
}
