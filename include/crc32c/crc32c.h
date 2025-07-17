#pragma once

#include <cstdint>
#include <cstddef>

namespace crc32c {

// Simple CRC32C implementation (software fallback)
uint32_t Crc32c(const char* data, size_t length);

// Extend CRC32C with additional data
uint32_t Extend(uint32_t crc, const char* data, size_t length);

} // namespace crc32c