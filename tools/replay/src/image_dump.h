// Minimal dependency-free PNG writer for replay frame dumps.
#pragma once

#include <cstdint>
#include <string>

namespace nhl::replay {

// Writes an 8-bit RGBA image (row-major, width*height*4 bytes) as a PNG using
// uncompressed (stored) DEFLATE blocks — no zlib dependency. Returns false on
// I/O error.
bool WritePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);

}  // namespace nhl::replay
