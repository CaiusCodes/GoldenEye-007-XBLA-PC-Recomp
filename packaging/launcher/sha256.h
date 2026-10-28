// Streaming SHA-256 (FIPS 180-4), self-contained, no OS dependency.
//
// Written for the launcher in the style of the public-domain reference
// implementations (Brad Conte's crypto-algorithms sha256.c, and the FIPS 180-4
// specification it follows); the algorithm and its constants are public domain.
//
//   ge::Sha256 sha;
//   sha.Add(data, size);   // any number of times
//   std::string hex = sha.Hex();   // 64 uppercase hex digits; finishes the hash
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ge {

class Sha256 {
 public:
  Sha256() { Reset(); }

  void Reset() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, kInit, sizeof(state_));
    bytes_ = 0;
    used_ = 0;
  }

  void Add(const uint8_t* data, size_t size) {
    bytes_ += size;
    if (used_ > 0) {
      const size_t take = size < 64 - used_ ? size : 64 - used_;
      std::memcpy(block_ + used_, data, take);
      used_ += take;
      data += take;
      size -= take;
      if (used_ < 64) return;
      Compress(block_);
      used_ = 0;
    }
    for (; size >= 64; data += 64, size -= 64) Compress(data);
    if (size > 0) {
      std::memcpy(block_, data, size);
      used_ = size;
    }
  }

  // The digest as 64 uppercase hex digits. The hash is finished by this call
  // (call Reset() before reusing the object).
  std::string Hex() {
    const uint64_t bit_length = bytes_ * 8;
    block_[used_++] = 0x80;
    if (used_ > 56) {
      std::memset(block_ + used_, 0, 64 - used_);
      Compress(block_);
      used_ = 0;
    }
    std::memset(block_ + used_, 0, 56 - used_);
    for (int i = 0; i < 8; ++i) block_[56 + i] = static_cast<uint8_t>(bit_length >> (56 - 8 * i));
    Compress(block_);
    used_ = 0;
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
      for (int k = 0; k < 4; ++k) {
        const uint8_t byte = static_cast<uint8_t>(state_[i] >> (24 - 8 * k));
        out[i * 8 + k * 2] = kDigits[byte >> 4];
        out[i * 8 + k * 2 + 1] = kDigits[byte & 15];
      }
    }
    return out;
  }

 private:
  static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void Compress(const uint8_t* chunk) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(chunk[i * 4]) << 24) | (uint32_t(chunk[i * 4 + 1]) << 16) |
             (uint32_t(chunk[i * 4 + 2]) << 8) | uint32_t(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + ch + K[i] + w[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  uint32_t state_[8];
  uint8_t block_[64];
  uint64_t bytes_ = 0;
  size_t used_ = 0;
};

}  // namespace ge
