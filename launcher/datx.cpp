#include "datx.h"

#include "fileio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

namespace fs = std::filesystem;
using fileio::ReadAll;


// The name hash of the archive (all arithmetic wraps at 32 bits).
uint32_t NameHash(const std::string& name, uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (unsigned char ch : name) {
    uint32_t c = ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
    if (c == '\\') c = '/';
    a = uint32_t(a * b) % 0x7FFFFFFEu;
    r = (uint32_t(r * a) + c) % 0x7FFFFFFFu;
  }
  return r;
}

// One LZ4 block.
bool Lz4Block(const uint8_t* src, size_t n, uint8_t* dst, size_t dsize) {
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t token = src[i++];
    size_t lit = token >> 4;
    if (lit == 15) {
      uint8_t s;
      do {
        if (i >= n) return false;
        s = src[i++];
        lit += s;
      } while (s == 255);
    }
    if (i + lit > n || o + lit > dsize) return false;
    memcpy(dst + o, src + i, lit);
    i += lit;
    o += lit;
    if (i >= n) break;  // the last sequence has no match
    if (i + 2 > n) return false;
    const size_t offset = src[i] | (src[i + 1] << 8);
    i += 2;
    size_t match = token & 15;
    if (match == 15) {
      uint8_t s;
      do {
        if (i >= n) return false;
        s = src[i++];
        match += s;
      } while (s == 255);
    }
    match += 4;
    if (offset == 0 || offset > o || o + match > dsize) return false;
    for (size_t k = 0; k < match; ++k, ++o) dst[o] = dst[o - offset];
  }
  return o == dsize;
}

std::string VolumeName(unsigned vol) {
  char buf[32];
  snprintf(buf, sizeof buf, "game%03u.dat", vol);
  return buf;
}

}  // namespace

std::string DatxExtract(const fs::path& update_dir, const fs::path& base_dir, const std::string& path,
                        std::vector<uint8_t>& out) {
  std::vector<uint8_t> ndx;
  if (!ReadAll(update_dir / "game.ndx", ndx) && !ReadAll(base_dir / "game.ndx", ndx)) return "game.ndx not found";
  if (ndx.size() < 0x40 || memcmp(ndx.data(), "DATX", 4) != 0) return "game.ndx has an unexpected format";
  const uint64_t filetime = Rd64(&ndx[8]);
  const uint32_t seed_a = Rd32(&ndx[0x18]), seed_b = Rd32(&ndx[0x1C]);
  const uint32_t chunk_size = Rd32(&ndx[0x24]);
  const uint32_t files = Rd32(&ndx[0x38]);
  if (chunk_size == 0 || chunk_size > 0x40000 || files == 0 || files > 1000000) return "game.ndx has unexpected values";
  size_t p = 0x3C;
  const size_t half = ndx[p];
  p += 1 + 2 * half;
  if (p + 4 > ndx.size()) return "game.ndx is cut short";
  const size_t names = Rd32(&ndx[p]);
  p += 4 + names;
  if (p + 4 > ndx.size() || Rd32(&ndx[p]) != 0x12345678u) return "game.ndx marker not found";
  p += 4;
  if (p + size_t(files) * 32 > ndx.size()) return "game.ndx records are cut short";
  const uint8_t* rec = &ndx[p];

  const uint32_t want = NameHash(path, seed_a, seed_b);
  size_t lo = 0, hi = files;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint32_t h = Rd32(rec + mid * 32 + 12);
    if (h < want) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= files || Rd32(rec + lo * 32 + 12) != want) return "not in the archive: " + path;
  const uint64_t loc = Rd64(rec + lo * 32);
  const uint32_t size = Rd32(rec + lo * 32 + 8);
  // Indexes dated before 2014-08-20 pack the location as 8 / 56 bits, later ones as 16 / 48.
  const uint64_t old_style = (1408492800ull + 11644473600ull) * 10000000ull;
  const unsigned vol = filetime <= old_style ? unsigned(loc & 0xFF) : unsigned(loc & 0xFFFF);
  const uint64_t offset = filetime <= old_style ? (loc >> 8) : (loc >> 16);

  std::vector<uint8_t> dat;
  FILE* f = fileio::Open(update_dir / VolumeName(vol), "rb");
  if (!f) f = fileio::Open(base_dir / VolumeName(vol), "rb");
  if (!f) return "volume file missing: " + VolumeName(vol);

  std::string err;
  uint8_t head[0x24];
  if (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, "DATF", 4) != 0 || memcmp(head + 0x20, "CHNK", 4) != 0) {
    err = "volume file has an unexpected format";
  }
  uint32_t count = 0, vchunk = 0;
  std::vector<uint8_t> table;
  if (err.empty()) {
    count = Rd32(head + 0x18);
    vchunk = Rd32(head + 0x1C);
    if (vchunk != chunk_size || count == 0 || count > 4000000) err = "volume file has unexpected values";
  }
  if (err.empty()) {
    table.resize(size_t(count) * 4 + 4);
    if (fread(table.data(), 1, table.size(), f) != table.size() || memcmp(&table[size_t(count) * 4], "DATA", 4) != 0)
      err = "volume chunk table is damaged";
  }
  out.clear();
  if (err.empty()) {
    const uint64_t data_start = 0x24 + uint64_t(count) * 4 + 4;
    const uint64_t first = offset / chunk_size;
    const uint64_t last = size ? (offset + size - 1) / chunk_size : first;
    if (last >= count) err = "file lies outside its volume";
    uint64_t pos = data_start;
    for (uint64_t k = 0; err.empty() && k < first; ++k) pos += Rd32(&table[k * 4]);
    out.reserve(size);
    std::vector<uint8_t> raw, plain(chunk_size);
    for (uint64_t k = first; err.empty() && k <= last; ++k) {
      const uint32_t stored = Rd32(&table[k * 4]);
      if (stored == 0) {
        std::fill(plain.begin(), plain.end(), 0);
      } else {
        raw.resize(stored);
        if (fileio::Seek(f, pos) != 0 || fread(raw.data(), 1, stored, f) != stored) {
          err = "volume file is cut short";
          break;
        }
        if (stored == chunk_size) {
          memcpy(plain.data(), raw.data(), chunk_size);
        } else if (stored > 4 && Rd32(raw.data()) == chunk_size) {
          if (!Lz4Block(raw.data() + 4, stored - 4, plain.data(), chunk_size)) {
            err = "a chunk could not be unpacked";
            break;
          }
        } else {
          err = "a chunk has an unexpected format";
          break;
        }
      }
      pos += stored;
      const uint64_t from = k == first ? offset % chunk_size : 0;
      const uint64_t to = k == last ? (offset + size - 1) % chunk_size + 1 : chunk_size;
      if (size) out.insert(out.end(), plain.begin() + from, plain.begin() + to);
    }
  }
  fclose(f);
  if (!err.empty()) {
    out.clear();
    return err;
  }
  return out.size() == size ? "" : "extracted size does not match the index";
}

// Size a volume must have: header, chunk table and every stored chunk. 0 when the header cannot be read.
static uint64_t VolumeExpectedSize(const fs::path& path, uint64_t& actual) {
  FILE* f = fileio::Open(path, "rb");
  if (!f) return 0;
  uint64_t expected = 0;
  uint8_t head[0x24];
  actual = fileio::SizeOf(f);
  if (fread(head, 1, sizeof head, f) == sizeof head && memcmp(head, "DATF", 4) == 0 && memcmp(head + 0x20, "CHNK", 4) == 0) {
    const uint32_t count = Rd32(head + 0x18);
    if (count != 0 && count <= 4000000) {
      std::vector<uint8_t> table(size_t(count) * 4 + 4);
      if (fread(table.data(), 1, table.size(), f) == table.size() && memcmp(&table[size_t(count) * 4], "DATA", 4) == 0) {
        expected = 0x24 + uint64_t(count) * 4 + 4;
        for (uint32_t k = 0; k < count; ++k) expected += Rd32(&table[size_t(k) * 4]);
      }
    }
  }
  fclose(f);
  return expected;
}

std::string DatxCheckVolumes(const fs::path& update_dir, const fs::path& base_dir, unsigned& used,
                             std::vector<unsigned>& missing, std::vector<unsigned>* truncated) {
  std::vector<uint8_t> ndx;
  if (!ReadAll(update_dir / "game.ndx", ndx) && !ReadAll(base_dir / "game.ndx", ndx)) return "game.ndx not found";
  if (ndx.size() < 0x40 || memcmp(ndx.data(), "DATX", 4) != 0) return "game.ndx has an unexpected format";
  const uint64_t filetime = Rd64(&ndx[8]);
  const uint32_t files = Rd32(&ndx[0x38]);
  if (files == 0 || files > 1000000) return "game.ndx has unexpected values";
  size_t p = 0x3C;
  const size_t half = ndx[p];
  p += 1 + 2 * half;
  if (p + 4 > ndx.size()) return "game.ndx is cut short";
  const size_t names = Rd32(&ndx[p]);
  p += 4 + names;
  if (p + 4 > ndx.size() || Rd32(&ndx[p]) != 0x12345678u) return "game.ndx marker not found";
  p += 4;
  if (p + size_t(files) * 32 > ndx.size()) return "game.ndx records are cut short";
  const uint64_t old_style = (1408492800ull + 11644473600ull) * 10000000ull;
  std::vector<bool> seen(65536, false);
  for (uint32_t i = 0; i < files; ++i) {
    const uint64_t loc = Rd64(&ndx[p + size_t(i) * 32]);
    seen[filetime <= old_style ? unsigned(loc & 0xFF) : unsigned(loc & 0xFFFF)] = true;
  }
  used = 0;
  missing.clear();
  if (truncated) truncated->clear();
  for (unsigned vol = 0; vol < seen.size(); ++vol) {
    if (!seen[vol]) continue;
    ++used;
    const fs::path in_update = update_dir / VolumeName(vol), in_base = base_dir / VolumeName(vol);
    const bool have_update = fileio::Exists(in_update);
    if (!have_update && !fileio::Exists(in_base)) {
      missing.push_back(vol);
    } else if (truncated) {
      uint64_t actual = 0;
      const uint64_t expected = VolumeExpectedSize(have_update ? in_update : in_base, actual);
      if (expected != 0 && actual < expected) truncated->push_back(vol);
    }
  }
  return "";
}

bool DatxContains(const fs::path& update_dir, const fs::path& base_dir, const std::string& path) {
  std::vector<uint8_t> ndx;
  if (!ReadAll(update_dir / "game.ndx", ndx) && !ReadAll(base_dir / "game.ndx", ndx)) return false;
  if (ndx.size() < 0x40 || memcmp(ndx.data(), "DATX", 4) != 0) return false;
  const uint32_t seed_a = Rd32(&ndx[0x18]), seed_b = Rd32(&ndx[0x1C]);
  const uint32_t files = Rd32(&ndx[0x38]);
  if (files == 0 || files > 1000000) return false;
  size_t p = 0x3C;
  const size_t half = ndx[p];
  p += 1 + 2 * half;
  if (p + 4 > ndx.size()) return false;
  const size_t names = Rd32(&ndx[p]);
  p += 4 + names;
  if (p + 4 > ndx.size() || Rd32(&ndx[p]) != 0x12345678u) return false;
  p += 4;
  if (p + size_t(files) * 32 > ndx.size()) return false;
  const uint8_t* rec = &ndx[p];
  const uint32_t want = NameHash(path, seed_a, seed_b);
  size_t lo = 0, hi = files;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (Rd32(rec + mid * 32 + 12) < want) lo = mid + 1;
    else hi = mid;
  }
  return lo < files && Rd32(rec + lo * 32 + 12) == want;
}
