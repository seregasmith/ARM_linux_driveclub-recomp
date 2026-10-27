#include "carswitch.h"

#include "fileio.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "datx.h"

namespace {

constexpr const char* kVersion = "carswitch-1";

// The car select blurs the showroom before it asks for the next car (Button_Clear_Bloom, 15 time units = 250 ms) and
// only clears the blur, and stops the loading spinner, when a second animation ends (Button_Bloom_Clear: holds for
// 30 units, then fades for 15). The car itself is ready long before that. Time units are 1/60 s.
constexpr float kBlurOut = 3;  // was 15
constexpr float kBlurIn = 5;   // was 45 (30 hold + 15 fade)

const char* const kFiles[3] = {"data/leveldata/canada_landscape_gui.rpk", "data/leveldata/chile_landscape_gui.rpk",
                               "data/leveldata/uk_landscape_gui_n.rpk"};

using fileio::ReadText;
using fileio::Widen;
bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  return fileio::WriteAll(path, data.data(), data.size());
}

// ------------------------------------------------------------------------------------------------ animation sequences
// A sequence in the menu file: its name, an 8-byte hash, "Time" with start and end, a fixed header, two colour
// channels of three curves each and four single curves. Every curve has two keys of six floats (time, value,
// tangents). Strings are a 32-bit length and the text. The three showroom files share the layout byte for byte.
struct Reader {
  const std::vector<uint8_t>& d;
  size_t p;
  bool ok = true;
  uint32_t U32() {
    if (p + 4 > d.size()) { ok = false; return 0; }
    uint32_t v;
    memcpy(&v, &d[p], 4);
    p += 4;
    return v;
  }
  float F32() {
    const uint32_t u = U32();
    float f;
    memcpy(&f, &u, 4);
    return f;
  }
  bool Text(const char* want) {
    const uint32_t n = U32();
    if (!ok || n != strlen(want) || p + n > d.size() || memcmp(&d[p], want, n) != 0) return ok = false;
    p += n;
    return true;
  }
};

// Offsets of the sequence's end time and of its keys' times. False when the sequence is missing, appears twice or
// has another layout.
bool FindSequence(const std::vector<uint8_t>& d, const char* name, size_t& end_at, std::vector<size_t>& key_at) {
  std::string needle(4, '\0');
  const uint32_t len = uint32_t(strlen(name));
  memcpy(needle.data(), &len, 4);
  needle += name;
  const std::string all(d.begin(), d.end());
  const size_t begin = all.find(needle);
  if (begin == std::string::npos || all.find(needle, begin + 1) != std::string::npos) return false;
  Reader r{d, begin};
  r.Text(name);
  r.p += 8;  // hash
  r.Text("Time");
  r.F32();  // start
  end_at = r.p;
  r.F32();
  const uint32_t header[9] = {0, 0, 2, 4, 0, 0, 0, 0, 0};
  for (uint32_t h : header) r.ok = r.ok && r.U32() == h;
  key_at.clear();
  const auto curve = [&](const std::string& label) {
    r.Text(label.c_str());
    r.ok = r.ok && r.U32() == 2;
    for (const char* key : {"0", "1"}) {
      r.Text(key);
      key_at.push_back(r.p);
      r.p += 6 * 4;
    }
    r.ok = r.ok && r.p <= d.size() && r.U32() == 0;
  };
  for (const char* group : {"HighDispersionColour", "LowDispersionColour"}) {
    r.Text(group);
    r.ok = r.ok && r.U32() == 3;
    for (const char* axis : {"_X", "_Y", "_Z"}) curve(std::string(group) + axis);
    r.ok = r.ok && r.U32() == 0;
  }
  for (const char* label : {"MasterBrightness", "NonBloomedAttenuation", "TemporalFade", "OverrideBlend"}) curve(label);
  return r.ok;
}

float FloatAt(const std::vector<uint8_t>& d, size_t at) {
  float f;
  memcpy(&f, &d[at], 4);
  return f;
}

void PutFloat(std::vector<uint8_t>& d, size_t at, float f) { memcpy(&d[at], &f, 4); }

// Moves a sequence's keys from `from` to `to` (two times per curve) and its end time from `from[1]` to `to[1]`.
// Only those floats change. False when the sequence does not have the expected times.
bool Retime(std::vector<uint8_t>& d, const char* name, const float from[2], const float to[2]) {
  size_t end_at;
  std::vector<size_t> key_at;
  if (!FindSequence(d, name, end_at, key_at) || key_at.size() != 20) return false;
  if (FloatAt(d, end_at) != from[1]) return false;
  for (size_t i = 0; i < key_at.size(); ++i) {
    if (FloatAt(d, key_at[i]) != from[i % 2]) return false;
  }
  PutFloat(d, end_at, to[1]);
  for (size_t i = 0; i < key_at.size(); ++i) PutFloat(d, key_at[i], to[i % 2]);
  return true;
}

}  // namespace

std::wstring BuildFastCarSwitch(const std::filesystem::path& update_dir, const std::filesystem::path& base_dir,
                                const std::filesystem::path& out_dir) {
  const std::filesystem::path stamp_path = out_dir / "stamp.txt";
  char stamp[256];
  snprintf(stamp, sizeof stamp, "%s %llx %llx", kVersion, (unsigned long long)fileio::Stamp(update_dir / "game.ndx"),
           (unsigned long long)fileio::Stamp(base_dir / "game.ndx"));
  const auto out_path = [&](const char* file) { return out_dir / std::filesystem::path(file).make_preferred(); };
  if (ReadText(stamp_path) == stamp) {
    bool all = true;
    for (const char* f : kFiles) all = all && fileio::Exists(out_path(f));
    if (all) return L"";
  }
  fileio::Remove(stamp_path);

  std::vector<std::vector<uint8_t>> out(3);
  for (int i = 0; i < 3; ++i) {
    const std::string err = DatxExtract(update_dir, base_dir, kFiles[i], out[i]);
    if (!err.empty()) return Widen(std::string(kFiles[i]) + ": " + err);
    const float out_from[2] = {0, 15}, out_to[2] = {0, kBlurOut};
    const float in_from[2] = {30, 45}, in_to[2] = {0, kBlurIn};
    if (!Retime(out[i], "Button_Clear_Bloom", out_from, out_to) || !Retime(out[i], "Button_Bloom_Clear", in_from, in_to))
      return Widen(std::string(kFiles[i]) + " has an unexpected layout");
  }
  for (int i = 0; i < 3; ++i) {
    if (!WriteFile(out_path(kFiles[i]), out[i])) return L"the menu files could not be written";
  }
  if (!WriteFile(stamp_path, std::vector<uint8_t>(stamp, stamp + strlen(stamp)))) return L"the menu files could not be written";
  return L"";
}
