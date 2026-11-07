// GoldenEye 007 XBLA PC Recomp - updates.
//
// Shared by the game (Help & Options > Online Settings > Check for Updates asks
// whether a newer release exists) and the launcher (GoldenEye 007.exe --update
// downloads it, checks its SHA-256 and replaces the program files).
// Header-only, C++17, Windows: WinHTTP for HTTPS (link winhttp.lib), CNG for
// SHA-256 (bcrypt.lib). Adapted from the Perfect Dark XBLA PC port's pd_update.h.
//
// The one source is this project's GitHub releases: the latest release's tag
// (vX.Y.Z) and its asset GoldenEye-007-XBLA-PC-Recomp-vX.Y.Z.zip, whose SHA-256
// GitHub reports with the asset ("digest": "sha256:..."). Without a digest
// nothing is downloaded. Nothing is sent but the request itself (User-Agent
// "GoldenEye 007/<version>"). GE_UPDATE_API (an environment variable, for
// testing) points both at another releases URL; plain http is allowed only for
// 127.0.0.1.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ge::update {

inline const wchar_t* const kReleasesApi =
    L"https://api.github.com/repos/CaiusCodes/GoldenEye-007-XBLA-PC-Recomp/releases/latest";
inline const char* const kZipPrefix = "GoldenEye-007-XBLA-PC-Recomp-v";

inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}
inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

inline std::string Lower(std::string s) {
  for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return s;
}

inline std::wstring ApiUrl() {
  wchar_t buffer[2048] = {};
  const DWORD n = GetEnvironmentVariableW(L"GE_UPDATE_API", buffer, 2048);
  return n > 0 && n < 2048 ? std::wstring(buffer, n) : std::wstring(kReleasesApi);
}

// The User-Agent: "GoldenEye 007/<version>".
inline std::wstring Agent(const std::string& version) { return L"GoldenEye 007/" + Widen(version); }

// ---------------------------------------------------------------- JSON --
// Just enough JSON for GitHub's release object.
struct Json {
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;
  const Json* Get(const char* key) const {
    for (const auto& f : fields)
      if (f.first == key) return &f.second;
    return nullptr;
  }
  std::string Str(const char* key) const {
    const Json* j = Get(key);
    return j && j->type == kString ? j->text : std::string();
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}
  bool Parse(Json& out) {
    if (!Value(out, 0)) return false;
    Space();
    return i_ == s_.size();
  }

 private:
  void Space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  bool Literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }
  static void Utf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
  }
  bool Hex4(uint32_t& v) {
    if (i_ + 4 > s_.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool String(std::string& out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t c1 = 0;
          if (!Hex4(c1)) return false;
          if (c1 >= 0xD800 && c1 < 0xDC00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            i_ += 2;
            uint32_t c2 = 0;
            if (!Hex4(c2)) return false;
            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          Utf8(out, c1);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool Value(Json& out, int depth) {
    if (depth > 64) return false;
    Space();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '{') {
      ++i_;
      out.type = Json::kObject;
      Space();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
      for (;;) {
        Space();
        std::string key;
        if (!String(key)) return false;
        Space();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        Json v;
        if (!Value(v, depth + 1)) return false;
        out.fields.emplace_back(std::move(key), std::move(v));
        Space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') { ++i_; continue; }
        if (s_[i_] == '}') return ++i_, true;
        return false;
      }
    }
    if (c == '[') {
      ++i_;
      out.type = Json::kArray;
      Space();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
      for (;;) {
        Json v;
        if (!Value(v, depth + 1)) return false;
        out.items.push_back(std::move(v));
        Space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') { ++i_; continue; }
        if (s_[i_] == ']') return ++i_, true;
        return false;
      }
    }
    if (c == '"') {
      out.type = Json::kString;
      return String(out.text);
    }
    if (Literal("true")) return out.type = Json::kBool, out.boolean = true, true;
    if (Literal("false")) return out.type = Json::kBool, true;
    if (Literal("null")) return out.type = Json::kNull, true;
    const size_t start = i_;
    while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) ++i_;
    if (i_ == start) return false;
    out.type = Json::kNumber;
    out.number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
    return true;
  }
  const std::string& s_;
  size_t i_ = 0;
};

// ------------------------------------------------------------ versions --
// Text with numbers in it, the numbers compared by value ("test9" < "test10").
inline int CompareNatural(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const bool da = a[i] >= '0' && a[i] <= '9', db = b[j] >= '0' && b[j] <= '9';
    if (da && db) {
      unsigned long long x = 0, y = 0;
      while (i < a.size() && a[i] >= '0' && a[i] <= '9') x = x * 10 + unsigned(a[i++] - '0');
      while (j < b.size() && b[j] >= '0' && b[j] <= '9') y = y * 10 + unsigned(b[j++] - '0');
      if (x != y) return x < y ? -1 : 1;
    } else {
      if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
      ++i, ++j;
    }
  }
  return i < a.size() ? 1 : j < b.size() ? -1 : 0;
}

// "0.9.7", "v0.9.7", "0.9.7a", "0.9.7a-test5": the numbers, then a letter
// (none < a < b), then a pre-release part after '-' (any pre-release is older
// than the same version without one). <0, 0, >0 like strcmp.
inline int CompareVersions(std::string a, std::string b) {
  auto split = [](std::string v, std::vector<int>& nums, std::string& letter, std::string& pre) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    const size_t dash = v.find('-');
    if (dash != std::string::npos) pre = v.substr(dash + 1), v = v.substr(0, dash);
    size_t i = 0;
    while (i < v.size()) {
      int n = 0;
      bool any = false;
      while (i < v.size() && v[i] >= '0' && v[i] <= '9') n = n * 10 + (v[i++] - '0'), any = true;
      if (any) nums.push_back(n);
      if (i < v.size() && v[i] == '.') { ++i; continue; }
      if (i < v.size()) { letter = v.substr(i); break; }
    }
    for (char& c : letter) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  };
  std::vector<int> na, nb;
  std::string la, lb, pa, pb;
  split(a, na, la, pa);
  split(b, nb, lb, pb);
  for (size_t i = 0; i < std::max(na.size(), nb.size()); ++i) {
    const int x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  if (const int c = CompareNatural(la, lb)) return c;
  if (pa.empty() != pb.empty()) return pa.empty() ? 1 : -1;
  return CompareNatural(Lower(pa), Lower(pb));
}

// ---------------------------------------------------------------- HTTP --
// GET `url` (following redirects; WinHTTP never follows HTTPS -> HTTP) into
// `body` or, with `file`, into that file. `progress(done, total)` (total 0 if
// unknown) may return false to stop.
inline bool HttpGet(const std::wstring& url, const std::wstring& agent, std::string* body, HANDLE file,
                    const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
  URL_COMPONENTS parts = {sizeof(parts)};
  wchar_t host[256] = {}, path[4096] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  wchar_t extra[4096] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 4096;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    error = "bad address";
    return false;
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && !(parts.nScheme == INTERNET_SCHEME_HTTP && std::wstring(host) == L"127.0.0.1")) {
    error = "only HTTPS addresses are allowed";
    return false;
  }
  HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)  // (before Windows 8.1)
    session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = "networking could not start";
    return false;
  }
  WinHttpSetTimeouts(session, 15000, 15000, 20000, 30000);
  // No cookies, no stored credentials: only the request itself goes out.
  DWORD no_cookies = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  bool ok = false;
  HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", (std::wstring(path) + extra).c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   secure ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  do {
    if (!request) {
      error = "could not reach the server";
      break;
    }
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies, sizeof(no_cookies));
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
      error = "could not reach GitHub (no internet connection?)";
      break;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      error = status == 404   ? "no release was found"
              : status == 403 || status == 429 ? "GitHub is busy (too many checks), try again in an hour"
                                               : "the server answered " + std::to_string(status);
      break;
    }
    uint64_t total = 0;
    wchar_t length[32] = {};
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                            &length_size, WINHTTP_NO_HEADER_INDEX))
      total = _wcstoui64(length, nullptr, 10);
    uint64_t done = 0;
    std::vector<char> chunk(1 << 16);
    ok = true;
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &got)) {
        error = "the download was interrupted";
        ok = false;
        break;
      }
      if (got == 0) break;
      if (file) {
        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), got, &written, nullptr) || written != got) {
          error = "could not write the download (disk full?)";
          ok = false;
          break;
        }
      } else if (body) {
        body->append(chunk.data(), got);
        if (body->size() > (8u << 20)) {
          error = "the answer was too large";
          ok = false;
          break;
        }
      }
      done += got;
      if (progress && !progress(done, total)) {
        error = "cancelled";
        ok = false;
        break;
      }
    }
    if (ok && total && done != total) {
      error = "the download was incomplete";
      ok = false;
    }
  } while (false);
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

// ------------------------------------------------------------- release --
struct Release {
  std::string version;   // "0.9.8" (the tag without its v)
  std::string page;      // the release's web page
  std::string zip_name, zip_url, sha256;  // sha256: 64 lowercase hex digits
  uint64_t zip_size = 0;
};

inline bool IsSha256(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// The latest release: its version and its ZIP with the ZIP's SHA-256 (from
// the asset's "digest"; a release without one is refused). When only the ZIP
// or its checksum is missing, out.version is still set, so a caller can tell
// the player they are up to date when that release is not newer anyway.
inline bool LatestRelease(Release& out, const std::wstring& agent, std::string& error) {
  std::string body;
  if (!HttpGet(ApiUrl(), agent, &body, nullptr, {}, error)) return false;
  Json root;
  if (!JsonParser(body).Parse(root) || root.type != Json::kObject) {
    error = "GitHub's answer could not be read";
    return false;
  }
  out = Release{};
  out.version = root.Str("tag_name");
  if (!out.version.empty() && (out.version[0] == 'v' || out.version[0] == 'V')) out.version.erase(0, 1);
  out.page = root.Str("html_url");
  if (out.version.empty()) {
    error = "the latest release has no version";
    return false;
  }
  for (char c : out.version) {  // it names a file below
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
          c == '_')) {
      out.version.clear();
      error = "the latest release's version could not be read";
      return false;
    }
  }
  const Json* assets = root.Get("assets");
  const std::string want = kZipPrefix + out.version + ".zip";
  if (assets && assets->type == Json::kArray) {
    for (const Json& a : assets->items) {
      if (a.Str("name") != want) continue;
      out.zip_name = want;
      out.zip_url = a.Str("browser_download_url");
      const Json* size = a.Get("size");
      if (size && size->type == Json::kNumber) out.zip_size = uint64_t(size->number);
      const std::string digest = Lower(a.Str("digest"));
      if (digest.rfind("sha256:", 0) == 0) out.sha256 = digest.substr(7);
    }
  }
  if (out.zip_url.empty()) {
    error = "release v" + out.version + " has no Windows download (" + want + ")";
    return false;
  }
  if (!IsSha256(out.sha256)) {
    error = "release v" + out.version + " has no checksum, so its download could not be checked";
    return false;
  }
  return true;
}

// The answer to "is there a newer release?": 1 newer (release is complete),
// 0 not newer, -1 could not tell (error says why). A latest release that is
// not newer counts as up to date even if its download is incomplete.
inline int CheckForNewer(Release& out, const std::wstring& agent, const std::string& current, std::string& error) {
  const bool ok = LatestRelease(out, agent, error);
  if (!out.version.empty() && CompareVersions(out.version, current) <= 0) return 0;
  return ok ? 1 : -1;
}

// SHA-256 of a file, as 64 lowercase hex digits ("" on failure).
inline std::string FileSha256(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string hex;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<unsigned char> buffer(1 << 20);
    DWORD got = 0;
    bool ok = true;
    for (;;) {
      if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &got, nullptr)) { ok = false; break; }
      if (!got) break;
      if (BCryptHashData(hash, buffer.data(), got, 0) != 0) { ok = false; break; }
    }
    unsigned char digest[32];
    if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
      static const char kHex[] = "0123456789abcdef";
      for (unsigned char b : digest) hex += kHex[b >> 4], hex += kHex[b & 15];
    }
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(file);
  return hex;
}

}  // namespace ge::update
