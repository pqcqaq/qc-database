#include "crc32c/crc32c.h"

namespace crc32c {

// CRC32C polynomial
static const uint32_t kCrc32cPoly = 0x82F63B78;

// Generate CRC32C lookup table
static uint32_t crc32c_table[256];
static bool table_initialized = false;

static void init_table() {
    if (table_initialized) return;

    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ kCrc32cPoly;
            } else {
                crc >>= 1;
            }
        }
        crc32c_table[i] = crc;
    }
    table_initialized = true;
}

uint32_t Crc32c(const char* data, size_t length) {
    init_table();

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = static_cast<uint8_t>(data[i]);
        crc = crc32c_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

uint32_t Extend(uint32_t crc, const char* data, size_t length) {
    init_table();

    crc ^= 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = static_cast<uint8_t>(data[i]);
        crc = crc32c_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

}  // namespace crc32c