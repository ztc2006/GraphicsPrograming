#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

struct GpuTimings {
  std::uint64_t frameId = 0;
  bool valid = false, clustered = false;
  double totalMs = 0, shadowMs = 0, mainMs = 0, outputMs = 0, uiMs = 0, cullingMs = 0, taaMs = 0;
};

inline double timestampMilliseconds(std::uint64_t start, std::uint64_t end,
                                    unsigned validBits, double periodNs) {
  if (validBits == 0 || validBits > 64 || periodNs <= 0)
    throw std::invalid_argument("Invalid timestamp capabilities");
  auto delta = end - start;
  if (validBits < 64)
    delta &= (std::uint64_t{1} << validBits) - 1;
  return static_cast<double>(delta) * periodNs / 1000000.0;
}

inline double percentile(std::vector<double> values, double p) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  auto const index = static_cast<std::size_t>(
      std::ceil(std::clamp(p, 0.0, 1.0) * values.size()));
  return values[index == 0 ? 0 : index - 1];
}

inline std::string jsonString(std::string const &value) {
  std::string result = "\"";
  char const *hex = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += c;
    } else if (c < 32) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else
      result += c;
  }
  return result + '"';
}
