// File helpers for the content builders (datx.cpp, unlock.cpp, graphics.cpp, carswitch.cpp), shared by the Windows
// launcher and Linux's dc-prepare. Paths are std::filesystem::path: the Windows launcher's std::wstring paths
// convert to them as they are.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fileio {

inline FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  wchar_t m[8] = {};
  for (size_t i = 0; mode[i] && i < 7; ++i) m[i] = wchar_t(mode[i]);
  return _wfopen(path.c_str(), m);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

inline int Seek(FILE* f, uint64_t pos) {
#ifdef _WIN32
  return _fseeki64(f, static_cast<long long>(pos), SEEK_SET);
#else
  return fseeko(f, static_cast<off_t>(pos), SEEK_SET);
#endif
}

// The size of an open file (its position goes back to the start), 0 when unknown.
inline uint64_t SizeOf(FILE* f) {
#ifdef _WIN32
  if (_fseeki64(f, 0, SEEK_END) != 0) return 0;
  const long long n = _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0) return 0;
  const long long n = ftello(f);
#endif
  Seek(f, 0);
  return n > 0 ? uint64_t(n) : 0;
}

inline bool Exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

inline void Remove(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

inline void MakeDirs(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
}

inline bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  FILE* f = Open(path, "rb");
  if (!f) return false;
  const uint64_t n = SizeOf(f);
  out.resize(size_t(n));
  const bool ok = n == 0 || fread(out.data(), 1, size_t(n), f) == size_t(n);
  fclose(f);
  return ok;
}

inline std::string ReadText(const std::filesystem::path& path) {
  FILE* f = Open(path, "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

// Writes the file, making its folder first.
inline bool WriteAll(const std::filesystem::path& path, const void* data, size_t size) {
  if (path.has_parent_path()) MakeDirs(path.parent_path());
  FILE* f = Open(path, "wb");
  if (!f) return false;
  const bool ok = size == 0 || fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

// A number that changes when the file is replaced (its size and modification time), 0 when it is missing.
inline uint64_t Stamp(const std::filesystem::path& path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return (uint64_t(d.nFileSizeLow) << 20) ^ uint64_t(d.ftLastWriteTime.dwLowDateTime) ^ (uint64_t(d.nFileSizeHigh) << 40);
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return (uint64_t(st.st_size) << 20) ^ uint64_t(st.st_mtim.tv_sec) * 1000000007ull ^ uint64_t(st.st_mtim.tv_nsec);
#endif
}

// Messages are built in UTF-8 and returned as the launcher's wide strings.
inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
#else
  std::wstring w;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
    w += wchar_t(cp);
    i += size_t(n);
  }
  return w;
#endif
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return "";
#ifdef _WIN32
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
#else
  std::string s;
  for (wchar_t wc : w) {
    const uint32_t c = uint32_t(wc);
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
  }
  return s;
#endif
}

}  // namespace fileio
