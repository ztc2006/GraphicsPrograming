#include "hdr_ibl_cache.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
void integer(Bytes &bytes, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    bytes.push_back(std::uint8_t(value >> (i * 8)));
}
void floating(Bytes &bytes, float value) {
  integer(bytes, std::bit_cast<std::uint32_t>(value), 4);
}
std::uint64_t hash(std::span<std::uint8_t const> bytes) {
  std::uint64_t value = 14695981039346656037ull;
  for (auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ull;
  }
  return value;
}
Bytes identity(HdrImage const &image, EnvironmentBakeSettings const &s) {
  Bytes bytes;
  bytes.reserve(28 + image.rgba.size() * 4);
  // Version includes cube orientation, source filter, BRDF, SH and RGBA32F.
  for (auto value : {environmentBakeVersion, image.width, image.height,
                     s.faceSize, s.prefilterSamples, s.lutSize, s.lutSamples})
    integer(bytes, value, 4);
  for (float value : image.rgba)
    floating(bytes, value);
  return bytes;
}
Bytes payload(BakedEnvironment const &data) {
  Bytes bytes;
  bytes.reserve(4 * (27 + data.cubeRgba.size() + data.brdfLut.rgba.size()));
  for (auto value : data.sh)
    for (unsigned c = 0; c < 3; ++c)
      floating(bytes, value[c]);
  for (float value : data.cubeRgba)
    floating(bytes, value);
  for (float value : data.brdfLut.rgba)
    floating(bytes, value);
  return bytes;
}
struct Reader {
  std::span<std::uint8_t const> bytes;
  std::size_t cursor = 0;
  std::uint64_t integer(unsigned count) {
    if (cursor > bytes.size() || count > bytes.size() - cursor)
      throw std::runtime_error("Truncated IBL cache");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
      value |= std::uint64_t(bytes[cursor++]) << (i * 8);
    return value;
  }
  float floating(bool signedValue = false) {
    float value = std::bit_cast<float>(std::uint32_t(integer(4)));
    if (!std::isfinite(value) || (!signedValue && value < 0))
      throw std::runtime_error("Invalid IBL cache value");
    return value;
  }
};
BakedEnvironment emptyBake(EnvironmentBakeSettings const &s) {
  BakedEnvironment data;
  for (auto size = s.faceSize;; size /= 2) {
    data.levels.push_back({size, data.cubeRgba.size()});
    data.cubeRgba.resize(data.cubeRgba.size() +
                         std::size_t(size) * size * 6 * 4);
    if (size == 1)
      break;
  }
  data.brdfLut = {
      .width = s.lutSize,
      .height = s.lutSize,
      .rgba = std::vector<float>(std::size_t(s.lutSize) * s.lutSize * 4)};
  return data;
}
bool read(std::filesystem::path const &path, Bytes const &key,
          BakedEnvironment &data) {
  auto payloadSize = 4 * (27 + data.cubeRgba.size() + data.brdfLut.rgba.size());
  auto expected = 32 + key.size() + payloadSize;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() != std::streampos(expected))
    return false;
  Bytes bytes(expected);
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    return false;
  try {
    Reader header{bytes};
    if (header.integer(8) != 0x313030304c424956ull || // "VIBL0001"
        header.integer(8) != key.size() || header.integer(8) != payloadSize)
      return false;
    auto checksum = header.integer(8);
    if (!std::equal(key.begin(), key.end(), bytes.begin() + 32))
      return false;
    auto body = std::span(bytes).subspan(32 + key.size());
    if (hash(body) != checksum)
      return false;
    Reader values{body};
    for (auto &value : data.sh)
      for (unsigned c = 0; c < 3; ++c)
        value[c] = values.floating(true);
    for (auto &value : data.cubeRgba)
      value = values.floating();
    for (auto &value : data.brdfLut.rgba)
      value = values.floating();
    return values.cursor == body.size();
  } catch (std::runtime_error const &) {
    return false;
  }
}
bool write(std::filesystem::path const &path, Bytes const &key,
           BakedEnvironment const &data) {
  // Atomic directory creation gives each writer an exclusive temporary file.
  // rename publishes a complete file; readers never observe a partial payload.
  static std::atomic<std::uint64_t> sequence{0};
  std::filesystem::path temporary;
  struct Cleanup {
    std::filesystem::path &path;
    ~Cleanup() {
      std::error_code e;
      if (!path.empty())
        std::filesystem::remove_all(path, e);
    }
  } cleanup{temporary};
  try {
    std::filesystem::create_directories(path.parent_path());
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      auto name =
          path.string() + ".tmp-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(sequence.fetch_add(1));
      if (std::filesystem::create_directory(name)) {
        temporary = name;
        break;
      }
    }
    if (temporary.empty())
      return false;
    auto body = payload(data);
    Bytes header;
    integer(header, 0x313030304c424956ull, 8);
    integer(header, key.size(), 8);
    integer(header, body.size(), 8);
    integer(header, hash(body), 8);
    std::ofstream file(temporary / "bake", std::ios::binary | std::ios::trunc);
    for (Bytes const *bytes :
         std::initializer_list<Bytes const *>{&header, &key, &body})
      file.write(reinterpret_cast<char const *>(bytes->data()), bytes->size());
    file.close();
    if (!file)
      return false;
    std::filesystem::rename(temporary / "bake", path);
    return true;
  } catch (std::filesystem::filesystem_error const &) {
    return false;
  }
}
} // namespace

EnvironmentBakeResult
loadOrBakeEnvironment(HdrImage const &image,
                      EnvironmentBakeSettings const &settings,
                      std::filesystem::path const &directory) {
  validateEnvironmentImage(image);
  validateEnvironmentBakeSettings(settings);
  if (directory.empty())
    return {.data = bakeEnvironment(image, settings)};
  auto key = identity(image, settings);
  std::ostringstream name;
  name << std::hex << std::setfill('0') << std::setw(16) << hash(key) << ".ibl";
  auto path = directory / name.str();
  auto data = emptyBake(settings);
  if (read(path, key, data))
    return {.data = std::move(data), .cacheHit = true};
  data = bakeEnvironment(image, settings);
  bool stored = write(path, key, data);
  return {.data = std::move(data), .cacheStored = stored};
}
