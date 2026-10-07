#include "texture_mip.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

void require(bool ok, char const *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
  try {
    f();
  } catch (std::runtime_error const &) {
    return;
  }
  throw std::runtime_error("Invalid mip input accepted");
}
int main() {
  try {
    std::array<unsigned char, 8> bw{0, 0, 0, 0, 255, 255, 255, 255};
    auto bytes = std::as_bytes(std::span(bw));
    auto color = generateTextureMips(bytes, 2, 1, TextureColorSpace::Srgb);
    auto data = generateTextureMips(bytes, 2, 1, TextureColorSpace::Linear);
    require(color.levels.size() == 2 && color.pixels.size() == 12,
            "Mip shape/layout");
    for (unsigned c = 0; c < 3; ++c) {
      require(color.pixels[8 + c] == std::byte{188},
              "sRGB averaged in encoded space");
      require(data.pixels[8 + c] == std::byte{128},
              "Data channels received gamma");
    }
    require(color.pixels[11] == std::byte{128}, "Alpha received gamma");
    std::array<unsigned char, 8> packed{0, 255, 10, 0, 255, 0, 50, 255};
    auto mr = generateTextureMips(std::as_bytes(std::span(packed)), 2, 1,
                                  TextureColorSpace::Linear);
    require(mr.pixels[8] == std::byte{128} && mr.pixels[9] == std::byte{128} &&
                mr.pixels[10] == std::byte{30} &&
                mr.pixels[11] == std::byte{128},
            "Packed channels mixed");
    std::array<unsigned char, 8> normals{255, 128, 128, 10, 128, 128, 255, 30};
    auto n = generateTextureMips(std::as_bytes(std::span(normals)), 2, 1,
                                 TextureColorSpace::Linear,
                                 TextureMipPolicy::Normal);
    double length2 = 0;
    for (unsigned c = 0; c < 3; ++c) {
      double v = std::to_integer<int>(n.pixels[8 + c]) / 255.0 * 2 - 1;
      length2 += v * v;
    }
    require(std::abs(std::sqrt(length2) - 1) < .01 &&
                n.pixels[11] == std::byte{74},
            "Normal mip lost unit direction/length-loss metadata");
    std::array<unsigned char, 8> opposed{255, 128, 128, 255, 0, 127, 127, 255};
    auto zero = generateTextureMips(std::as_bytes(std::span(opposed)), 2, 1,
                                    TextureColorSpace::Linear,
                                    TextureMipPolicy::Normal);
    require(zero.pixels[8] == std::byte{128} &&
                zero.pixels[9] == std::byte{128} &&
                zero.pixels[10] == std::byte{255},
            "Degenerate normal fallback");
    // Independent source-space first-moment reference, including non-unit
    // encoded vectors. No generation helper contributes to this oracle.
    auto checkMoments = [&](unsigned w, unsigned h,
                            std::vector<std::byte> pixels) {
      std::array<double, 3> mean{};
      for (unsigned i = 0; i < w * h; ++i) {
        std::array<double, 3> n{};
        for (unsigned c = 0; c < 3; ++c)
          n[c] = 2.0 * std::to_integer<int>(pixels[i * 4 + c]) / 255 - 1;
        double norm = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (unsigned c = 0; c < 3; ++c)
          mean[c] += n[c] / norm / (w * h);
      }
      double length =
          std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
      auto chain = generateTextureMips(pixels, w, h, TextureColorSpace::Linear,
                                       TextureMipPolicy::Normal);
      for (unsigned i = 0; i < w * h; ++i) {
        for (unsigned c = 0; c < 3; ++c)
          require(chain.pixels[i * 4 + c] == pixels[i * 4 + c],
                  "Normal LOD0 RGB changed");
        require(chain.pixels[i * 4 + 3] == std::byte{0},
                "Authored normal alpha leaked into variance");
      }
      auto offset = chain.levels.back().offset;
      double loss = std::to_integer<unsigned>(chain.pixels[offset + 3]) / 255.0;
      require(std::abs(loss - (1 - length)) <= .5001 / 255,
              "Hierarchical normal moments lost variance/precision");
      if (length > 1e-6)
        for (unsigned c = 0; c < 3; ++c) {
          double actual =
              2.0 * std::to_integer<unsigned>(chain.pixels[offset + c]) / 255 -
              1;
          require(std::abs(actual - mean[c] / length) <= 1.001 / 255,
                  "Normal first-moment direction differs");
        }
      return chain;
    };
    for (auto shape : {std::array{8u, 8u}, std::array{3u, 5u},
                       std::array{1u, 9u}, std::array{9u, 1u}}) {
      std::vector<std::byte> pixels(shape[0] * shape[1] * 4);
      // Unequal child lengths: renormalizing intermediate means would change
      // both the final direction and loss. Alpha deliberately unrelated.
      std::array<unsigned char, 12> dirs{255, 128, 128, 128, 128, 255,
                                         128, 255, 128, 100, 160, 220};
      for (unsigned i = 0; i < shape[0] * shape[1]; ++i) {
        for (unsigned c = 0; c < 3; ++c)
          pixels[i * 4 + c] = std::byte(dirs[(i % 4) * 3 + c]);
        pixels[i * 4 + 3] = std::byte(i * 17 % 256);
      }
      checkMoments(shape[0], shape[1], pixels);
      for (unsigned i = 0; i < shape[0] * shape[1]; ++i)
        for (unsigned c = 0; c < 3; ++c)
          pixels[i * 4 + c] = std::byte(dirs[9 + c]);
      auto flat = checkMoments(shape[0], shape[1], pixels);
      for (auto const &m : flat.levels)
        for (auto i = m.offset + 3; i < m.offset + m.size; i += 4)
          require(flat.pixels[i] == std::byte{0},
                  "Constant tilted normal acquired variance");
    }
    require(zero.pixels[11] == std::byte{255},
            "Opposed normal distribution lost full variance");
    require(textureMipAlgorithmVersion == 3,
            "Normal metadata cache not versioned");
    auto layout = textureMipLayout(3, 5, 4, true);
    require(layout == std::vector<TextureMipLevel>{{3, 5, 0, 60},
                                                   {1, 2, 60, 8},
                                                   {1, 1, 68, 4}},
            "Odd extent or offset");
    for (auto dimensions :
         {std::array{3u, 5u}, std::array{1u, 5u}, std::array{5u, 1u}}) {
      std::vector<std::byte> pixels(dimensions[0] * dimensions[1] * 4,
                                    std::byte{0});
      // Last column/row must contribute rather than disappear in floor/2.
      for (unsigned y = 0; y < dimensions[1]; ++y)
        for (unsigned x = 0; x < dimensions[0]; ++x)
          if (dimensions[0] == 1 ? y == dimensions[1] - 1
                                 : x == dimensions[0] - 1)
            for (unsigned c = 0; c < 4; ++c)
              pixels[(y * dimensions[0] + x) * 4 + c] = std::byte{255};
      auto odd = generateTextureMips(pixels, dimensions[0], dimensions[1],
                                     TextureColorSpace::Linear);
      int expected =
          dimensions[0] == 1 ? 51 : int(std::round(255.0 / dimensions[0]));
      require(
          std::abs(std::to_integer<int>(odd.pixels[odd.levels.back().offset]) -
                   expected) <= 1,
          "Odd edge texels lost");
    }
    auto base = generateTextureMips(bytes, 2, 1, TextureColorSpace::Srgb,
                                    TextureMipPolicy::BaseOnly);
    require(base.levels.size() == 1 &&
                base.pixels ==
                    std::vector<std::byte>(bytes.begin(), bytes.end()),
            "BaseOnly changed mask source");
    // Half of LOD 0 passes, but every ordinary 2x2 average falls below .5.
    // Different block intensities permit an exact 8/16 coverage at LOD 1.
    std::vector<std::byte> mask(8 * 8 * 4);
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        unsigned block = (y / 2) * 4 + x / 2;
        auto index = (y * 8 + x) * 4;
        mask[index] = std::byte{51};
        mask[index + 1] = std::byte{102};
        mask[index + 2] = std::byte{153};
        mask[index + 3] = std::byte(x % 2 ? block * 2 : 160 + block);
      }
    auto ordinary = generateTextureMips(mask, 8, 8, TextureColorSpace::Srgb);
    auto covered = generateTextureMips(mask, 8, 8, TextureColorSpace::Srgb,
                                       TextureMipPolicy::AlphaCoverage);
    auto passing = [](TextureMipChain const &chain, unsigned level,
                      unsigned channel, float cutoff) {
      unsigned count = 0;
      auto const &m = chain.levels[level];
      for (auto i = m.offset + channel; i < m.offset + m.size; i += 4)
        count +=
            float(std::to_integer<unsigned>(chain.pixels[i])) / 255 >= cutoff;
      return count;
    };
    require(passing(ordinary, 1, 3, .5f) == 0 &&
                passing(covered, 1, 3, .5f) == 8,
            "Alpha mips lost source coverage");
    require(std::equal(mask.begin(), mask.end(), covered.pixels.begin()),
            "Coverage changed LOD 0");
    for (auto const &m : covered.levels)
      for (auto i = m.offset; i < m.offset + m.size; i += 4)
        require(covered.pixels[i] == std::byte{51} &&
                    covered.pixels[i + 1] == std::byte{102} &&
                    covered.pixels[i + 2] == std::byte{153},
                "Coverage contaminated color channels");
    for (auto cutoff : {.3f, .5f, .7f}) {
      auto adjusted = generateTextureMips(mask, 8, 8, TextureColorSpace::Linear,
                                          TextureMipPolicy::AlphaCoverage,
                                          {.cutoff = cutoff});
      for (unsigned level = 1; level < adjusted.levels.size(); ++level) {
        auto count = adjusted.levels[level].size / 4;
        double source = double(passing(adjusted, 0, 3, cutoff)) / 64;
        double error = std::abs(
            double(passing(adjusted, level, 3, cutoff)) / count - source);
        require(error <= std::max(.02, 1.0 / count),
                "Representative coverage exceeds texel discretization");
      }
    }
    for (unsigned i = 0; i < 64; ++i) {
      mask[i * 4] = mask[i * 4 + 3];
      mask[i * 4 + 3] = std::byte{255};
    }
    auto red = generateTextureMips(mask, 8, 8, TextureColorSpace::Linear,
                                   TextureMipPolicy::AlphaCoverage,
                                   {.channel = TextureAlphaChannel::Red});
    require(passing(red, 1, 0, .5f) == 8 && passing(red, 1, 3, 1) == 16,
            "Separate opacity map did not preserve linear R/opaque A");
    // Uniform ties have a discrete jump: a two-texel chain cannot retain .5
    // coverage in its 1x1 mip. Prefer scale one among equally good answers.
    auto tied = generateTextureMips(bytes, 2, 1, TextureColorSpace::Linear,
                                    TextureMipPolicy::AlphaCoverage);
    require(tied.pixels[11] == std::byte{128},
            "Coverage tie unnecessarily changed alpha");
    rejects([&] {
      generateTextureMips(mask, 8, 8, TextureColorSpace::Linear,
                          TextureMipPolicy::AlphaCoverage,
                          {.cutoff = std::numeric_limits<float>::quiet_NaN()});
    });
    rejects([&] {
      generateTextureMips(mask, 8, 8, TextureColorSpace::Srgb,
                          TextureMipPolicy::AlphaCoverage,
                          {.channel = TextureAlphaChannel::Red});
    });
    rejects(
        [&] { generateTextureMips(bytes, 0, 1, TextureColorSpace::Linear); });
    rejects(
        [&] { generateTextureMips(bytes, 2, 2, TextureColorSpace::Linear); });
    rejects([&] {
      generateTextureMips(bytes, 2, 1, TextureColorSpace::Srgb,
                          TextureMipPolicy::Normal);
    });
    rejects([] { textureMipLayout(UINT32_MAX, UINT32_MAX, 4, true); });
    rejects([] { textureMipLayout(1, 1, 0, true); });
    std::cout << "PASS typed mips: sRGB/linear/alpha, packed channels, "
                 "normal first moments/length loss/flat/opposed/LOD0/version, "
                 "odd/long extents, layout/overflow, "
                 "BaseOnly/quantized alpha coverage/threshold/channel/ties\n";
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
