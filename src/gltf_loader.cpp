#include "pch.hpp"

#include "gltf_loader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

namespace {
class JsonValue {
public:
  enum class Type {
    eNull,
    eBool,
    eNumber,
    eString,
    eArray,
    eObject,
  };

  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;

  Type type = Type::eNull;
  bool boolValue = false;
  double numberValue = 0.0;
  std::string stringValue;
  Array arrayValue;
  Object objectValue;

  JsonValue const *find(std::string const &key) const {
    auto iter = objectValue.find(key);
    if (iter == objectValue.end()) {
      return nullptr;
    }
    return &iter->second;
  }

  JsonValue const &at(std::string const &key) const {
    JsonValue const *value = find(key);
    if (value == nullptr) {
      throw std::runtime_error("glTF JSON is missing required key: " + key);
    }
    return *value;
  }

  bool isArray() const { return type == Type::eArray; }
  bool isObject() const { return type == Type::eObject; }

  std::string asString(std::string fallback = {}) const {
    return type == Type::eString ? stringValue : fallback;
  }

  double asNumber(double fallback = 0.0) const {
    return type == Type::eNumber ? numberValue : fallback;
  }

  int asInt(int fallback = 0) const {
    return type == Type::eNumber ? static_cast<int>(numberValue) : fallback;
  }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view source) : source_(source) {}

  JsonValue parse() {
    JsonValue value = parseValue();
    skipWhitespace();
    if (position_ != source_.size()) {
      throw std::runtime_error("Unexpected trailing data in glTF JSON.");
    }
    return value;
  }

private:
  JsonValue parseValue() {
    skipWhitespace();
    if (position_ >= source_.size()) {
      throw std::runtime_error("Unexpected end of glTF JSON.");
    }

    char const c = source_[position_];
    if (c == '{') {
      return parseObject();
    }
    if (c == '[') {
      return parseArray();
    }
    if (c == '"') {
      JsonValue value{};
      value.type = JsonValue::Type::eString;
      value.stringValue = parseString();
      return value;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
      return parseNumber();
    }
    if (matchLiteral("true")) {
      JsonValue value{};
      value.type = JsonValue::Type::eBool;
      value.boolValue = true;
      return value;
    }
    if (matchLiteral("false")) {
      JsonValue value{};
      value.type = JsonValue::Type::eBool;
      value.boolValue = false;
      return value;
    }
    if (matchLiteral("null")) {
      return {};
    }
    throw std::runtime_error("Invalid token in glTF JSON.");
  }

  JsonValue parseObject() {
    expect('{');
    JsonValue value{};
    value.type = JsonValue::Type::eObject;
    skipWhitespace();
    if (tryConsume('}')) {
      return value;
    }

    while (true) {
      skipWhitespace();
      std::string key = parseString();
      skipWhitespace();
      expect(':');
      value.objectValue.emplace(std::move(key), parseValue());
      skipWhitespace();
      if (tryConsume('}')) {
        break;
      }
      expect(',');
    }
    return value;
  }

  JsonValue parseArray() {
    expect('[');
    JsonValue value{};
    value.type = JsonValue::Type::eArray;
    skipWhitespace();
    if (tryConsume(']')) {
      return value;
    }

    while (true) {
      value.arrayValue.push_back(parseValue());
      skipWhitespace();
      if (tryConsume(']')) {
        break;
      }
      expect(',');
    }
    return value;
  }

  JsonValue parseNumber() {
    std::size_t start = position_;
    if (source_[position_] == '-') {
      ++position_;
    }
    consumeDigits();
    if (position_ < source_.size() && source_[position_] == '.') {
      ++position_;
      consumeDigits();
    }
    if (position_ < source_.size() &&
        (source_[position_] == 'e' || source_[position_] == 'E')) {
      ++position_;
      if (source_[position_] == '+' || source_[position_] == '-') {
        ++position_;
      }
      consumeDigits();
    }

    double parsed = 0.0;
    auto const numberText = source_.substr(start, position_ - start);
    auto [ptr, ec] = std::from_chars(
        numberText.data(), numberText.data() + numberText.size(), parsed);
    if (ec != std::errc{}) {
      throw std::runtime_error("Invalid number in glTF JSON.");
    }
    JsonValue value{};
    value.type = JsonValue::Type::eNumber;
    value.numberValue = parsed;
    return value;
  }

  std::string parseString() {
    expect('"');
    std::string result;
    while (position_ < source_.size()) {
      char const c = source_[position_++];
      if (c == '"') {
        return result;
      }
      if (c != '\\') {
        result.push_back(c);
        continue;
      }

      if (position_ >= source_.size()) {
        throw std::runtime_error("Invalid string escape in glTF JSON.");
      }
      char const escaped = source_[position_++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        result.push_back(escaped);
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      default:
        throw std::runtime_error("Unsupported string escape in glTF JSON.");
      }
    }
    throw std::runtime_error("Unterminated string in glTF JSON.");
  }

  void skipWhitespace() {
    while (position_ < source_.size() &&
           std::isspace(static_cast<unsigned char>(source_[position_])) != 0) {
      ++position_;
    }
  }

  void consumeDigits() {
    while (position_ < source_.size() &&
           std::isdigit(static_cast<unsigned char>(source_[position_])) != 0) {
      ++position_;
    }
  }

  bool matchLiteral(std::string_view literal) {
    if (source_.substr(position_, literal.size()) != literal) {
      return false;
    }
    position_ += literal.size();
    return true;
  }

  bool tryConsume(char c) {
    if (position_ >= source_.size() || source_[position_] != c) {
      return false;
    }
    ++position_;
    return true;
  }

  void expect(char c) {
    skipWhitespace();
    if (position_ >= source_.size() || source_[position_] != c) {
      throw std::runtime_error("Unexpected character in glTF JSON.");
    }
    ++position_;
  }

  std::string_view source_;
  std::size_t position_ = 0;
};

struct BufferView {
  std::size_t buffer = 0;
  std::size_t byteOffset = 0;
  std::size_t byteLength = 0;
  std::size_t byteStride = 0;
};

struct Accessor {
  std::size_t bufferView = 0;
  std::size_t byteOffset = 0;
  int componentType = 0;
  std::size_t count = 0;
  std::string type;
};

struct PrimitiveRef {
  MeshId meshId = 0;
  MaterialId materialId = 0;
};

std::string lowercase(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  return -1;
}

std::string percentDecode(std::string_view encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    char const c = encoded[index];
    if (c != '%') {
      decoded.push_back(c);
      continue;
    }
    if (index + 2 >= encoded.size()) {
      throw std::runtime_error("Invalid percent-encoded glTF URI.");
    }
    int const high = hexDigit(encoded[index + 1]);
    int const low = hexDigit(encoded[index + 2]);
    if (high < 0 || low < 0) {
      throw std::runtime_error("Invalid percent-encoded glTF URI.");
    }
    decoded.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }
  return decoded;
}

std::filesystem::path resolveCaseInsensitivePath(std::filesystem::path path) {
  if (std::filesystem::exists(path)) {
    return path;
  }

  std::filesystem::path const parent = path.parent_path();
  if (parent.empty() || !std::filesystem::exists(parent)) {
    return path;
  }

  std::string const wanted = lowercase(path.filename().string());
  for (std::filesystem::directory_entry const &entry :
       std::filesystem::directory_iterator(parent)) {
    if (lowercase(entry.path().filename().string()) == wanted) {
      return entry.path();
    }
  }
  return path;
}

std::filesystem::path
resolveGltfAssetPath(std::filesystem::path const &gltfPath,
                     std::string const &uri) {
  if (uri.starts_with("data:")) {
    return {};
  }

  std::string relative = percentDecode(uri);
  std::replace(relative.begin(), relative.end(), '\\', '/');
  return resolveCaseInsensitivePath(gltfPath.parent_path() /
                                    std::filesystem::path{relative});
}

MaterialId resolveMaterialId(int materialIndex, std::size_t materialCount,
                             std::filesystem::path const &path) {
  if (materialIndex < 0 ||
      static_cast<std::size_t>(materialIndex) >= materialCount) {
    std::cerr << "glTF primitive references out-of-range material index "
              << materialIndex << " in " << path.string()
              << ", falling back to material 0\n";
    return 0;
  }
  return static_cast<MaterialId>(materialIndex);
}

std::string readTextFile(std::filesystem::path const &path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("Failed to open glTF file: " + path.string());
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

std::vector<std::byte> readBinaryFile(std::filesystem::path const &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Failed to open glTF buffer: " + path.string());
  }
  auto const size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  file.read(reinterpret_cast<char *>(data.data()), size);
  return data;
}

std::vector<std::byte> decodeBase64(std::string_view encoded) {
  std::array<int, 256> table{};
  table.fill(-1);
  std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t index = 0; index < alphabet.size(); ++index) {
    table[static_cast<unsigned char>(alphabet[index])] =
        static_cast<int>(index);
  }

  std::vector<std::byte> result;
  int value = 0;
  int bits = -8;
  for (char c : encoded) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if (c == '=') {
      break;
    }
    int const decoded = table[static_cast<unsigned char>(c)];
    if (decoded < 0) {
      throw std::runtime_error("Invalid base64 data in glTF buffer URI.");
    }
    value = (value << 6) + decoded;
    bits += 6;
    if (bits >= 0) {
      result.push_back(static_cast<std::byte>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return result;
}

std::vector<std::byte> loadBuffer(std::filesystem::path const &gltfPath,
                                  JsonValue const &buffer) {
  std::string const uri = buffer.at("uri").asString();
  std::string_view const dataPrefix = "data:";
  if (uri.starts_with(dataPrefix)) {
    std::string_view const base64Marker = ";base64,";
    std::size_t const markerPos = uri.find(base64Marker);
    if (markerPos == std::string::npos) {
      throw std::runtime_error(
          "Only base64-encoded glTF data URIs are supported.");
    }
    return decodeBase64(
        std::string_view(uri).substr(markerPos + base64Marker.size()));
  }

  std::filesystem::path bufferPath = resolveGltfAssetPath(gltfPath, uri);
  return readBinaryFile(bufferPath);
}

std::size_t componentSize(int componentType) {
  switch (componentType) {
  case 5120:
  case 5121:
    return 1;
  case 5122:
  case 5123:
    return 2;
  case 5125:
  case 5126:
    return 4;
  default:
    throw std::runtime_error("Unsupported glTF accessor component type.");
  }
}

std::size_t componentCount(std::string const &type) {
  if (type == "SCALAR") {
    return 1;
  }
  if (type == "VEC2") {
    return 2;
  }
  if (type == "VEC3") {
    return 3;
  }
  if (type == "VEC4") {
    return 4;
  }
  throw std::runtime_error("Unsupported glTF accessor type.");
}

std::span<std::byte const>
accessorBytes(Accessor const &accessor,
              std::vector<BufferView> const &bufferViews,
              std::vector<std::vector<std::byte>> const &buffers) {
  BufferView const &view = bufferViews.at(accessor.bufferView);
  std::vector<std::byte> const &buffer = buffers.at(view.buffer);
  std::size_t const start = view.byteOffset + accessor.byteOffset;
  if (start > buffer.size()) {
    throw std::runtime_error("glTF accessor points outside buffer.");
  }
  return {buffer.data() + start, buffer.size() - start};
}

float readFloat(std::span<std::byte const> data, std::size_t offset) {
  if (offset + sizeof(float) > data.size()) {
    throw std::runtime_error("glTF float accessor read is out of bounds.");
  }
  float value = 0.0f;
  std::memcpy(&value, data.data() + offset, sizeof(float));
  return value;
}

glm::vec2 readVec2(Accessor const &accessor,
                   std::vector<BufferView> const &bufferViews,
                   std::vector<std::vector<std::byte>> const &buffers,
                   std::size_t index) {
  if (accessor.componentType != 5126 || accessor.type != "VEC2") {
    throw std::runtime_error("glTF TEXCOORD_0 accessor must be float VEC2.");
  }
  BufferView const &view = bufferViews.at(accessor.bufferView);
  std::size_t const stride =
      view.byteStride == 0 ? sizeof(float) * 2 : view.byteStride;
  auto data = accessorBytes(accessor, bufferViews, buffers);
  std::size_t const offset = index * stride;
  return {readFloat(data, offset + sizeof(float) * 0),
          readFloat(data, offset + sizeof(float) * 1)};
}

glm::vec3 readVec3(Accessor const &accessor,
                   std::vector<BufferView> const &bufferViews,
                   std::vector<std::vector<std::byte>> const &buffers,
                   std::size_t index, char const *semantic) {
  if (accessor.componentType != 5126 || accessor.type != "VEC3") {
    throw std::runtime_error(std::string("glTF ") + semantic +
                             " accessor must be float VEC3.");
  }
  BufferView const &view = bufferViews.at(accessor.bufferView);
  std::size_t const stride =
      view.byteStride == 0 ? sizeof(float) * 3 : view.byteStride;
  auto data = accessorBytes(accessor, bufferViews, buffers);
  std::size_t const offset = index * stride;
  return {readFloat(data, offset + sizeof(float) * 0),
          readFloat(data, offset + sizeof(float) * 1),
          readFloat(data, offset + sizeof(float) * 2)};
}

std::vector<std::uint32_t>
readIndices(Accessor const &accessor,
            std::vector<BufferView> const &bufferViews,
            std::vector<std::vector<std::byte>> const &buffers) {
  if (accessor.type != "SCALAR") {
    throw std::runtime_error("glTF index accessor must be SCALAR.");
  }
  BufferView const &view = bufferViews.at(accessor.bufferView);
  std::size_t const stride = view.byteStride == 0
                                 ? componentSize(accessor.componentType)
                                 : view.byteStride;
  auto data = accessorBytes(accessor, bufferViews, buffers);
  std::vector<std::uint32_t> indices;
  indices.reserve(accessor.count);

  for (std::size_t index = 0; index < accessor.count; ++index) {
    std::size_t const offset = index * stride;
    if (accessor.componentType == 5121) {
      indices.push_back(static_cast<std::uint8_t>(data[offset]));
    } else if (accessor.componentType == 5123) {
      std::uint16_t value = 0;
      std::memcpy(&value, data.data() + offset, sizeof(value));
      indices.push_back(value);
    } else if (accessor.componentType == 5125) {
      std::uint32_t value = 0;
      std::memcpy(&value, data.data() + offset, sizeof(value));
      indices.push_back(value);
    } else {
      throw std::runtime_error("Unsupported glTF index component type.");
    }
  }
  return indices;
}

std::vector<BufferView> parseBufferViews(JsonValue const &root) {
  std::vector<BufferView> views;
  JsonValue const &bufferViews = root.at("bufferViews");
  views.reserve(bufferViews.arrayValue.size());
  for (JsonValue const &view : bufferViews.arrayValue) {
    views.push_back(BufferView{
        .buffer = static_cast<std::size_t>(view.at("buffer").asInt()),
        .byteOffset = static_cast<std::size_t>(
            view.find("byteOffset") != nullptr ? view.at("byteOffset").asInt()
                                               : 0),
        .byteLength = static_cast<std::size_t>(view.at("byteLength").asInt()),
        .byteStride = static_cast<std::size_t>(
            view.find("byteStride") != nullptr ? view.at("byteStride").asInt()
                                               : 0),
    });
  }
  return views;
}

std::vector<Accessor> parseAccessors(JsonValue const &root) {
  std::vector<Accessor> accessors;
  JsonValue const &jsonAccessors = root.at("accessors");
  accessors.reserve(jsonAccessors.arrayValue.size());
  for (JsonValue const &accessor : jsonAccessors.arrayValue) {
    accessors.push_back(Accessor{
        .bufferView =
            static_cast<std::size_t>(accessor.at("bufferView").asInt()),
        .byteOffset =
            static_cast<std::size_t>(accessor.find("byteOffset") != nullptr
                                         ? accessor.at("byteOffset").asInt()
                                         : 0),
        .componentType = accessor.at("componentType").asInt(),
        .count = static_cast<std::size_t>(accessor.at("count").asInt()),
        .type = accessor.at("type").asString(),
    });
  }
  return accessors;
}

std::vector<std::vector<std::byte>>
parseBuffers(std::filesystem::path const &path, JsonValue const &root) {
  std::vector<std::vector<std::byte>> buffers;
  JsonValue const &jsonBuffers = root.at("buffers");
  buffers.reserve(jsonBuffers.arrayValue.size());
  for (JsonValue const &buffer : jsonBuffers.arrayValue) {
    buffers.push_back(loadBuffer(path, buffer));
  }
  return buffers;
}

glm::vec4 parseVec4(JsonValue const *value, glm::vec4 fallback) {
  if (value == nullptr || !value->isArray() || value->arrayValue.size() != 4) {
    return fallback;
  }
  return {static_cast<float>(value->arrayValue[0].asNumber()),
          static_cast<float>(value->arrayValue[1].asNumber()),
          static_cast<float>(value->arrayValue[2].asNumber()),
          static_cast<float>(value->arrayValue[3].asNumber())};
}

std::vector<std::string> parseImageUris(JsonValue const &root) {
  std::vector<std::string> uris;
  JsonValue const *images = root.find("images");
  if (images == nullptr) {
    return uris;
  }
  uris.reserve(images->arrayValue.size());
  for (JsonValue const &image : images->arrayValue) {
    uris.push_back(image.find("uri") != nullptr ? image.at("uri").asString()
                                                : std::string{});
  }
  return uris;
}

std::vector<int> parseTextureSources(JsonValue const &root) {
  std::vector<int> sources;
  JsonValue const *textures = root.find("textures");
  if (textures == nullptr) {
    return sources;
  }
  sources.reserve(textures->arrayValue.size());
  for (JsonValue const &texture : textures->arrayValue) {
    sources.push_back(
        texture.find("source") != nullptr ? texture.at("source").asInt() : -1);
  }
  return sources;
}

std::string resolveGltfTexturePath(std::filesystem::path const &path,
                                   std::vector<std::string> const &imageUris,
                                   std::vector<int> const &textureSources,
                                   int textureIndex, char const *textureKind) {
  if (textureIndex < 0 ||
      static_cast<std::size_t>(textureIndex) >= textureSources.size()) {
    return {};
  }

  int const imageIndex = textureSources[textureIndex];
  if (imageIndex < 0 ||
      static_cast<std::size_t>(imageIndex) >= imageUris.size() ||
      imageUris[imageIndex].empty()) {
    return {};
  }

  std::filesystem::path texturePath =
      resolveGltfAssetPath(path, imageUris[imageIndex]);
  if (texturePath.empty()) {
    std::cerr << "glTF " << textureKind
              << " texture uses embedded image data URI; ignoring it\n";
    return {};
  }
  if (!std::filesystem::exists(texturePath)) {
    std::cerr << "glTF " << textureKind
              << " texture not found: " << imageUris[imageIndex] << '\n';
    return {};
  }
  return texturePath.string();
}

std::vector<Material> parseMaterials(JsonValue const &root,
                                     std::filesystem::path const &path,
                                     std::string fallbackAlbedoPath) {
  std::vector<std::string> imageUris = parseImageUris(root);
  std::vector<int> textureSources = parseTextureSources(root);

  std::vector<Material> materials;
  JsonValue const *jsonMaterials = root.find("materials");
  if (jsonMaterials == nullptr) {
    materials.push_back(Material{
        .albedoPath = std::move(fallbackAlbedoPath),
        .tint = {1.0f, 1.0f, 1.0f, 1.0f},
    });
    return materials;
  }

  materials.reserve(jsonMaterials->arrayValue.size());
  for (JsonValue const &jsonMaterial : jsonMaterials->arrayValue) {
    Material material{
        .albedoPath = fallbackAlbedoPath,
        .tint = {1.0f, 1.0f, 1.0f, 1.0f},
    };

    JsonValue const *pbr = jsonMaterial.find("pbrMetallicRoughness");
    if (pbr != nullptr) {
      material.tint = parseVec4(pbr->find("baseColorFactor"), material.tint);
      JsonValue const *baseColorTexture = pbr->find("baseColorTexture");
      if (baseColorTexture != nullptr &&
          baseColorTexture->find("index") != nullptr) {
        std::string const texturePath = resolveGltfTexturePath(
            path, imageUris, textureSources,
            baseColorTexture->at("index").asInt(-1), "base color");
        if (!texturePath.empty()) {
          material.albedoPath = texturePath;
        }
      }
    }

    JsonValue const *alphaMode = jsonMaterial.find("alphaMode");
    if (alphaMode != nullptr) {
      std::string const mode = alphaMode->asString("OPAQUE");
      if (mode == "MASK") {
        material.alphaMode = AlphaMode::Mask;
      } else if (mode == "BLEND") {
        material.alphaMode = AlphaMode::Blend;
      } else {
        material.alphaMode = AlphaMode::Opaque;
      }
    }
    if (material.alphaMode == AlphaMode::Mask) {
      JsonValue const *alphaCutoff = jsonMaterial.find("alphaCutoff");
      if (alphaCutoff != nullptr) {
        material.alphaCutoff =
            static_cast<float>(alphaCutoff->asNumber(material.alphaCutoff));
      }
    }

    JsonValue const *normalTexture = jsonMaterial.find("normalTexture");
    if (normalTexture != nullptr && normalTexture->find("index") != nullptr) {
      material.normalPath = resolveGltfTexturePath(
          path, imageUris, textureSources,
          normalTexture->at("index").asInt(-1), "normal");
      if (normalTexture->find("scale") != nullptr) {
        material.normalScale =
            static_cast<float>(normalTexture->at("scale").asNumber(1.0));
      }
    }
    materials.push_back(std::move(material));
  }
  return materials;
}

Aabb emptyBounds() {
  return {
      .min = glm::vec3{std::numeric_limits<float>::max()},
      .max = glm::vec3{std::numeric_limits<float>::lowest()},
      .valid = false,
  };
}

void includePoint(Aabb &bounds, glm::vec3 point) {
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
  bounds.valid = true;
}

Mesh loadPrimitive(JsonValue const &primitive,
                   std::vector<Accessor> const &accessors,
                   std::vector<BufferView> const &bufferViews,
                   std::vector<std::vector<std::byte>> const &buffers) {
  JsonValue const &attributes = primitive.at("attributes");
  int const positionAccessorIndex = attributes.at("POSITION").asInt();
  int const normalAccessorIndex = attributes.find("NORMAL") != nullptr
                                      ? attributes.at("NORMAL").asInt()
                                      : -1;
  int const uvAccessorIndex = attributes.find("TEXCOORD_0") != nullptr
                                  ? attributes.at("TEXCOORD_0").asInt()
                                  : -1;

  Accessor const &positionAccessor = accessors.at(positionAccessorIndex);
  Accessor const *normalAccessor =
      normalAccessorIndex >= 0 ? &accessors.at(normalAccessorIndex) : nullptr;
  Accessor const *uvAccessor =
      uvAccessorIndex >= 0 ? &accessors.at(uvAccessorIndex) : nullptr;

  Mesh mesh{};
  mesh.vertices.reserve(positionAccessor.count);
  mesh.localBounds = emptyBounds();
  for (std::size_t index = 0; index < positionAccessor.count; ++index) {
    glm::vec3 const position =
        readVec3(positionAccessor, bufferViews, buffers, index, "POSITION");
    glm::vec3 const normal =
        normalAccessor != nullptr
            ? readVec3(*normalAccessor, bufferViews, buffers, index, "NORMAL")
            : glm::vec3{0.0f, 0.0f, 1.0f};
    glm::vec2 const uv =
        uvAccessor != nullptr
            ? readVec2(*uvAccessor, bufferViews, buffers, index)
            : glm::vec2{0.0f};
    mesh.vertices.push_back(Vertex{
        .position = position,
        .color = {1.0f, 1.0f, 1.0f},
        .normal = normal,
        .uv = uv,
    });
    includePoint(mesh.localBounds, position);
  }

  if (primitive.find("indices") != nullptr) {
    mesh.indices = readIndices(accessors.at(primitive.at("indices").asInt()),
                               bufferViews, buffers);
  } else {
    mesh.indices.reserve(mesh.vertices.size());
    for (std::uint32_t index = 0; index < mesh.vertices.size(); ++index) {
      mesh.indices.push_back(index);
    }
  }

  generateMeshTangents(mesh);
  return mesh;
}

glm::mat4 parseNodeLocalMatrix(JsonValue const &node) {
  if (node.find("matrix") != nullptr) {
    JsonValue const &matrix = node.at("matrix");
    if (!matrix.isArray() || matrix.arrayValue.size() != 16) {
      throw std::runtime_error("glTF node matrix must contain 16 numbers.");
    }
    glm::mat4 result{1.0f};
    for (std::size_t index = 0; index < 16; ++index) {
      result[index % 4][index / 4] =
          static_cast<float>(matrix.arrayValue[index].asNumber());
    }
    return result;
  }

  glm::vec3 translation{0.0f};
  if (node.find("translation") != nullptr) {
    JsonValue const &value = node.at("translation");
    translation = {static_cast<float>(value.arrayValue[0].asNumber()),
                   static_cast<float>(value.arrayValue[1].asNumber()),
                   static_cast<float>(value.arrayValue[2].asNumber())};
  }

  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  if (node.find("rotation") != nullptr) {
    JsonValue const &value = node.at("rotation");
    rotation = glm::quat{static_cast<float>(value.arrayValue[3].asNumber()),
                         static_cast<float>(value.arrayValue[0].asNumber()),
                         static_cast<float>(value.arrayValue[1].asNumber()),
                         static_cast<float>(value.arrayValue[2].asNumber())};
  }

  glm::vec3 scale{1.0f};
  if (node.find("scale") != nullptr) {
    JsonValue const &value = node.at("scale");
    scale = {static_cast<float>(value.arrayValue[0].asNumber()),
             static_cast<float>(value.arrayValue[1].asNumber()),
             static_cast<float>(value.arrayValue[2].asNumber())};
  }

  return glm::translate(glm::mat4{1.0f}, translation) * glm::toMat4(rotation) *
         glm::scale(glm::mat4{1.0f}, scale);
}

Transform decomposeTransform(glm::mat4 const &matrix) {
  glm::vec3 scale;
  glm::quat orientation;
  glm::vec3 translation;
  glm::vec3 skew;
  glm::vec4 perspective;
  if (!glm::decompose(matrix, scale, orientation, translation, skew,
                      perspective)) {
    throw std::runtime_error("Failed to decompose glTF node transform.");
  }

  return Transform{
      .translation = translation,
      .rotation = glm::eulerAngles(glm::conjugate(orientation)),
      .scale = scale,
  };
}

Aabb transformBounds(Aabb const &localBounds, glm::mat4 const &matrix) {
  if (!localBounds.valid) {
    return {};
  }

  Aabb world = emptyBounds();
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        glm::vec3 corner{
            x == 0 ? localBounds.min.x : localBounds.max.x,
            y == 0 ? localBounds.min.y : localBounds.max.y,
            z == 0 ? localBounds.min.z : localBounds.max.z,
        };
        includePoint(world, glm::vec3(matrix * glm::vec4(corner, 1.0f)));
      }
    }
  }
  return world;
}

void collectNodeObjects(JsonValue const &nodes, std::size_t nodeIndex,
                        glm::mat4 const &parentTransform,
                        std::vector<std::vector<PrimitiveRef>> const &meshRefs,
                        std::vector<Mesh> const &loadedMeshes,
                        std::vector<SceneObject> &objects) {
  JsonValue const &node = nodes.arrayValue.at(nodeIndex);
  glm::mat4 const worldTransform = parentTransform * parseNodeLocalMatrix(node);

  if (node.find("mesh") != nullptr) {
    std::size_t const meshIndex =
        static_cast<std::size_t>(node.at("mesh").asInt());
    for (PrimitiveRef const &primitive : meshRefs.at(meshIndex)) {
      SceneObject object{};
      object.transform = decomposeTransform(worldTransform);
      object.meshId = primitive.meshId;
      object.materialId = primitive.materialId;
      object.worldBounds =
          transformBounds(loadedMeshes.at(primitive.meshId).localBounds,
                          object.transform.matrix());
      objects.push_back(object);
    }
  }

  if (node.find("children") != nullptr) {
    for (JsonValue const &child : node.at("children").arrayValue) {
      collectNodeObjects(nodes, static_cast<std::size_t>(child.asInt()),
                         worldTransform, meshRefs, loadedMeshes, objects);
    }
  }
}
} // namespace

LoadedScene loadStaticGltfScene(std::filesystem::path const &path,
                                std::string fallbackAlbedoPath) {
  std::string const jsonText = readTextFile(path);
  JsonValue const root = JsonParser(jsonText).parse();

  std::vector<std::vector<std::byte>> buffers = parseBuffers(path, root);
  std::vector<BufferView> bufferViews = parseBufferViews(root);
  std::vector<Accessor> accessors = parseAccessors(root);

  LoadedScene result{};
  result.materials = parseMaterials(root, path, std::move(fallbackAlbedoPath));
  if (result.materials.empty()) {
    throw std::runtime_error("glTF import produced no materials.");
  }

  JsonValue const &jsonMeshes = root.at("meshes");
  std::vector<std::vector<PrimitiveRef>> meshRefs;
  meshRefs.reserve(jsonMeshes.arrayValue.size());
  for (JsonValue const &jsonMesh : jsonMeshes.arrayValue) {
    std::vector<PrimitiveRef> refs;
    for (JsonValue const &primitive : jsonMesh.at("primitives").arrayValue) {
      if (primitive.find("mode") != nullptr &&
          primitive.at("mode").asInt() != 4) {
        throw std::runtime_error(
            "Only glTF triangle primitives are supported.");
      }
      MaterialId const materialId =
          primitive.find("material") != nullptr
              ? resolveMaterialId(primitive.at("material").asInt(),
                                  result.materials.size(), path)
              : 0;
      MeshId const meshId = static_cast<MeshId>(result.meshes.size());
      result.meshes.push_back(
          loadPrimitive(primitive, accessors, bufferViews, buffers));
      refs.push_back(PrimitiveRef{
          .meshId = meshId,
          .materialId = materialId,
      });
    }
    meshRefs.push_back(std::move(refs));
  }

  JsonValue const &nodes = root.at("nodes");
  int sceneIndex = root.find("scene") != nullptr ? root.at("scene").asInt() : 0;
  JsonValue const &scene = root.at("scenes").arrayValue.at(sceneIndex);
  for (JsonValue const &nodeIndex : scene.at("nodes").arrayValue) {
    collectNodeObjects(nodes, static_cast<std::size_t>(nodeIndex.asInt()),
                       glm::mat4{1.0f}, meshRefs, result.meshes,
                       result.objects);
  }

  if (result.meshes.empty() || result.objects.empty()) {
    throw std::runtime_error("glTF import produced no drawable objects.");
  }
  return result;
}
