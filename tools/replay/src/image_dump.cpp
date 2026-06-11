#include "image_dump.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace nhl::replay {

namespace {

uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[n] = c;
    }
    init = true;
  }
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

uint32_t Adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void WriteChunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& data) {
  PutBE32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> typed(tag, tag + 4);
  typed.insert(typed.end(), data.begin(), data.end());
  out.insert(out.end(), typed.begin(), typed.end());
  PutBE32(out, Crc32(typed.data(), typed.size()) ^ 0xFFFFFFFFu);
}

}  // namespace

bool WritePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  // Filtered raw scanlines: a 0x00 (filter=none) byte then width*4 RGBA bytes.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0x00);
    const uint8_t* row = rgba + static_cast<size_t>(y) * width * 4;
    raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
  }

  // zlib stream with stored (uncompressed) DEFLATE blocks.
  std::vector<uint8_t> zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    const size_t block = (raw.size() - off < 65535) ? (raw.size() - off) : 65535;
    const bool last = (off + block) >= raw.size();
    zlib.push_back(last ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
    const uint16_t len = static_cast<uint16_t>(block);
    zlib.push_back(uint8_t(len & 0xFF));
    zlib.push_back(uint8_t(len >> 8));
    const uint16_t nlen = static_cast<uint16_t>(~len);
    zlib.push_back(uint8_t(nlen & 0xFF));
    zlib.push_back(uint8_t(nlen >> 8));
    zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + block);
    off += block;
  }
  PutBE32(zlib, Adler32(raw.data(), raw.size()));

  std::vector<uint8_t> out = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

  std::vector<uint8_t> ihdr;
  PutBE32(ihdr, width);
  PutBE32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // color type RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  WriteChunk(out, "IHDR", ihdr);
  WriteChunk(out, "IDAT", zlib);
  WriteChunk(out, "IEND", {});

  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
  const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

}  // namespace nhl::replay
