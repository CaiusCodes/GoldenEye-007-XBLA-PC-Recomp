// SPDX-License-Identifier: GPL-3.0-only
//
// GoldenEye 007 XBLA PC Recomp - Xbox 360 STFS (LIVE/PIRS/CON) package reader.
// See stfs.cpp for the licence notice.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace stfs {

// ASCII case-insensitive comparison (package and folder names are ASCII).
inline bool EqualsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

struct Entry {
  std::string name;
  std::filesystem::path path;  // relative path inside the package, composed with fs::path
  bool directory = false;
  bool consecutive = false;
  uint32_t blocks = 0;
  uint32_t first_block = 0;
  int parent = -1;
  uint32_t size = 0;
};

// A package opened read-only (memory mapped). The file is never written.
class Package {
 public:
  Package() = default;
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;

  // Maps the file and reads its file table. On failure returns false and sets
  // *error to a sentence for the player.
  bool Open(const std::filesystem::path& path, std::string* error);
  void Close();

  const std::vector<Entry>& entries() const { return entries_; }
  // Case-insensitive; `path` uses '/' as the separator ("files/loc/x.str").
  const Entry* Find(const std::string& path) const;
  uint64_t total_file_bytes() const;
  // The title ID and content type from the header (valid after Open).
  uint32_t title_id() const { return title_id_; }
  uint32_t content_type() const { return content_type_; }

  // Streams a file's data to sink in pieces of at most 4 KB. Stops (and
  // returns false) when sink returns false or the block chain is damaged.
  bool Read(const Entry& entry, const std::function<bool(const uint8_t*, size_t)>& sink,
            std::string* error) const;

 private:
  uint64_t BlockToAddress(uint32_t block) const;
  bool NextBlock(uint32_t block, uint32_t* next) const;

#ifdef _WIN32
  void* file_ = nullptr;     // file handle
  void* mapping_ = nullptr;  // file mapping handle
#else
  int file_fd_ = -1;
#endif
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
  int shift_ = 0;
  uint64_t first_hash_table_ = 0;
  uint32_t title_id_ = 0;
  uint32_t content_type_ = 0;
  std::vector<Entry> entries_;
};

}  // namespace stfs
