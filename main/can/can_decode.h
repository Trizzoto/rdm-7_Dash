/*
 * can_decode.h — Pure, stateless CAN bit-field extraction.
 *
 * Zero LVGL, FreeRTOS, or ESP-IDF dependencies.
 * Safe to unit-test on a host machine.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * Extract a bit field from a CAN frame payload.
 *
 * @param data       Raw CAN data bytes (up to 8 bytes).
 * @param bit_offset Bit position of the first bit.
 * @param bit_length Number of bits to extract (1–64).
 * @param endian     0 = big-endian (Motorola), 1 = little-endian (Intel).
 * @param is_signed  Sign-extend the result when true.
 * @return           Extracted value as a signed 64-bit integer.
 */
int64_t can_extract_bits(const uint8_t *data, uint8_t bit_offset,
                         uint8_t bit_length, int endian, bool is_signed);

/**
 * Pack a value into a CAN frame payload at the given bit position.
 *
 * @param data       CAN data buffer (8 bytes, caller must zero-init).
 * @param bit_offset Bit position of the first bit (0-63).
 * @param bit_length Number of bits (1-32).
 * @param value      Value to pack.
 * @param endian     0 = big-endian (Motorola), 1 = little-endian (Intel).
 */
void can_pack_bits(uint8_t *data, uint8_t bit_offset,
                   uint8_t bit_length, uint32_t value, int endian);
