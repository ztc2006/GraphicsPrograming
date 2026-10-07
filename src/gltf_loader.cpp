#include "gltf_loader.hpp"

#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {
[[noreturn]] void invalid(std::string const &message) {
  throw std::runtime_error("glTF: " + message);
}
void check(cgltf_result result, char const *stage) {
  if (result != cgltf_result_success)
    invalid(std::string(stage) + " failed (code " + std::to_string(result) +
            "; check buffer/accessor/texture/sampler references).");
}
void range(std::size_t offset, std::size_t count, std::size_t stride,
           std::size_t element, std::size_t size) {
  // Subtractions/division keep hostile offsets/counts from wrapping before
  // validation.
  if (!count || !element || stride < element || offset > size ||
      element > size - offset ||
      (count - 1) > (size - offset - element) / stride)
    invalid("accessor range exceeds bufferView.");
}
bool unsignedComponent(cgltf_component_type type) {
  return type == cgltf_component_type_r_8u ||
         type == cgltf_component_type_r_16u ||
         type == cgltf_component_type_r_32u;
}
std::uint32_t readUnsigned(void const *bytes, cgltf_component_type type) {
  // memcpy also handles sparse indices without imposing host pointer alignment.
  std::uint32_t value = 0;
  switch (type) {
  case cgltf_component_type_r_8u: {
    std::uint8_t v;
    std::memcpy(&v, bytes, 1);
    value = v;
    break;
  }
  case cgltf_component_type_r_16u: {
    std::uint16_t v;
    std::memcpy(&v, bytes, 2);
    value = v;
    break;
  }
  case cgltf_component_type_r_32u:
    std::memcpy(&value, bytes, 4);
    break;
  default:
    invalid("indices must be unsigned byte, short or int.");
  }
  return value;
}
void validateStorage(cgltf_data const &data) {
  for (std::size_t i = 0; i < data.buffer_views_count; ++i) {
    auto const &view = data.buffer_views[i];
    if (view.has_meshopt_compression)
      invalid("EXT_meshopt_compression decoding is not implemented.");
    if (!view.buffer || !view.buffer->data || view.offset > view.buffer->size ||
        view.size > view.buffer->size - view.offset)
      invalid("bufferView range exceeds its buffer.");
  }
  for (std::size_t i = 0; i < data.accessors_count; ++i) {
    auto const &a = data.accessors[i];
    auto const component = cgltf_component_size(a.component_type);
    auto const element = cgltf_calc_size(a.type, a.component_type);
    if (!component || !element || !a.count ||
        a.count > std::numeric_limits<std::uint32_t>::max())
      invalid("invalid accessor type/component/count.");
    if (a.buffer_view) {
      range(a.offset, a.count, a.stride, element, a.buffer_view->size);
      if ((a.buffer_view->offset + a.offset) % component ||
          a.stride % component)
        invalid("misaligned accessor.");
    } else if (a.offset) {
      invalid("accessor without bufferView has a byteOffset.");
    }
    if (!a.is_sparse)
      continue;
    auto const &s = a.sparse;
    if (!unsignedComponent(s.indices_component_type) ||
        !s.indices_buffer_view || !s.values_buffer_view || s.count > a.count ||
        s.indices_buffer_view->stride || s.values_buffer_view->stride)
      invalid("invalid sparse accessor storage.");
    auto const indexSize = cgltf_component_size(s.indices_component_type);
    range(s.indices_byte_offset, s.count, indexSize, indexSize,
          s.indices_buffer_view->size);
    range(s.values_byte_offset, s.count, element, element,
          s.values_buffer_view->size);
    if ((s.indices_buffer_view->offset + s.indices_byte_offset) % indexSize ||
        (s.values_buffer_view->offset + s.values_byte_offset) % component)
      invalid("misaligned sparse values.");
    auto const *indices =
        cgltf_buffer_view_data(s.indices_buffer_view) + s.indices_byte_offset;
    std::uint32_t previous = 0;
    for (std::size_t j = 0; j < s.count; ++j) {
      auto const index =
          readUnsigned(indices + j * indexSize, s.indices_component_type);
      if (index >= a.count || (j && index <= previous))
        invalid("sparse indices must be increasing and within accessor count.");
      previous = index;
    }
  }
}
std::vector<float> unpack(cgltf_accessor const &a, cgltf_type type,
                          std::size_t count, char const *semantic) {
  if (a.type != type || a.count != count)
    invalid(std::string(semantic) + " type/count differs from POSITION.");
  auto const components = cgltf_num_components(type);
  if (count >
      std::numeric_limits<std::size_t>::max() / (components * sizeof(float)))
    invalid("accessor output is too large.");
  std::vector<float> values(count * components);
  // cgltf 1.15 advances sparse values with the base accessor stride. Sparse
  // values are tightly packed even when the base view contains interleaved
  // data.
  auto base = a;
  base.is_sparse = false;
  if (cgltf_accessor_unpack_floats(&base, values.data(), values.size()) !=
      values.size())
    invalid(std::string("cannot decode ") + semantic);
  if (a.is_sparse) {
    auto sparse = base;
    sparse.buffer_view = a.sparse.values_buffer_view;
    sparse.offset = a.sparse.values_byte_offset;
    sparse.count = a.sparse.count;
    sparse.stride = cgltf_calc_size(a.type, a.component_type);
    std::vector<float> overrides(sparse.count * components);
    if (cgltf_accessor_unpack_floats(&sparse, overrides.data(),
                                     overrides.size()) != overrides.size())
      invalid("cannot decode sparse values.");
    auto const size = cgltf_component_size(a.sparse.indices_component_type);
    auto const *indices = cgltf_buffer_view_data(a.sparse.indices_buffer_view) +
                          a.sparse.indices_byte_offset;
    for (std::size_t j = 0; j < sparse.count; ++j) {
      auto const index =
          readUnsigned(indices + j * size, a.sparse.indices_component_type);
      std::copy_n(overrides.data() + j * components, components,
                  values.data() + index * components);
    }
  }
  for (float value : values)
    if (!std::isfinite(value))
      invalid(std::string("non-finite ") + semantic);
  return values;
}
std::vector<std::uint32_t> unpackIndices(cgltf_accessor const &a) {
  if (a.type != cgltf_type_scalar || !unsignedComponent(a.component_type) ||
      a.normalized)
    invalid("invalid triangle index accessor.");
  std::vector<std::uint32_t> result(a.count, 0);
  auto const size = cgltf_component_size(a.component_type);
  if (a.buffer_view) {
    auto const *bytes = cgltf_buffer_view_data(a.buffer_view) + a.offset;
    for (std::size_t i = 0; i < a.count; ++i)
      result[i] = readUnsigned(bytes + i * a.stride, a.component_type);
  }
  if (a.is_sparse) {
    auto const indexSize =
        cgltf_component_size(a.sparse.indices_component_type);
    auto const *indices = cgltf_buffer_view_data(a.sparse.indices_buffer_view) +
                          a.sparse.indices_byte_offset;
    auto const *values = cgltf_buffer_view_data(a.sparse.values_buffer_view) +
                         a.sparse.values_byte_offset;
    for (std::size_t i = 0; i < a.sparse.count; ++i)
      result[readUnsigned(indices + i * indexSize,
                          a.sparse.indices_component_type)] =
          readUnsigned(values + i * size, a.component_type);
  }
  return result;
}
TextureSamplerDescription sampler(cgltf_sampler const *s) {
  TextureSamplerDescription result;
  if (!s)
    return result;
  switch (s->mag_filter) {
  case cgltf_filter_type_undefined:
    break;
  case cgltf_filter_type_nearest:
    result.mag = TextureFilter::Nearest;
    break;
  case cgltf_filter_type_linear:
    result.mag = TextureFilter::Linear;
    break;
  default:
    invalid("invalid sampler magFilter.");
  }
  switch (s->min_filter) {
  case cgltf_filter_type_undefined:
    break;
  case cgltf_filter_type_nearest:
    result.min = TextureFilter::Nearest;
    result.mip = TextureMipFilter::None;
    break;
  case cgltf_filter_type_linear:
    result.min = TextureFilter::Linear;
    result.mip = TextureMipFilter::None;
    break;
  case cgltf_filter_type_nearest_mipmap_nearest:
    result.min = TextureFilter::Nearest;
    result.mip = TextureMipFilter::Nearest;
    break;
  case cgltf_filter_type_linear_mipmap_nearest:
    result.min = TextureFilter::Linear;
    result.mip = TextureMipFilter::Nearest;
    break;
  case cgltf_filter_type_nearest_mipmap_linear:
    result.min = TextureFilter::Nearest;
    result.mip = TextureMipFilter::Linear;
    break;
  case cgltf_filter_type_linear_mipmap_linear:
    result.min = TextureFilter::Linear;
    result.mip = TextureMipFilter::Linear;
    break;
  default:
    invalid("invalid sampler minFilter.");
  }
  auto wrap = [](cgltf_wrap_mode mode) {
    switch (mode) {
    case cgltf_wrap_mode_repeat:
      return TextureWrap::Repeat;
    case cgltf_wrap_mode_clamp_to_edge:
      return TextureWrap::ClampToEdge;
    case cgltf_wrap_mode_mirrored_repeat:
      return TextureWrap::MirroredRepeat;
    default:
      invalid("invalid sampler wrap mode.");
    }
  };
  result.u = wrap(s->wrap_s);
  result.v = wrap(s->wrap_t);
  return result;
}
int texCoord(cgltf_texture_view const &view) {
  int set = view.has_transform && view.transform.has_texcoord
                ? view.transform.texcoord
                : view.texcoord;
  if (set < 0)
    invalid("negative texture coordinate set.");
  return set;
}
void image(cgltf_texture_view const &view, std::filesystem::path const &path,
           std::string &imagePath, std::vector<std::byte> &bytes) {
  if (!view.texture)
    return;
  auto const *source = view.texture->image;
  if (!source)
    invalid("texture has no supported image source.");
  if (source->buffer_view) {
    auto const *data = cgltf_buffer_view_data(source->buffer_view);
    bytes.resize(source->buffer_view->size);
    std::memcpy(bytes.data(), data, bytes.size());
  } else if (source->uri &&
             std::string_view{source->uri}.starts_with("data:")) {
    std::string_view uri{source->uri};
    auto const start = uri.find(";base64,");
    if (start == std::string_view::npos)
      invalid("image data URI must use base64.");
    auto encoded = uri.substr(start + 8);
    if (encoded.empty() || encoded.size() % 4)
      invalid("invalid image base64 length.");
    auto size = encoded.size() / 4 * 3;
    if (encoded.ends_with("="))
      --size;
    if (encoded.ends_with("=="))
      --size;
    cgltf_options options{};
    void *decoded = nullptr;
    check(cgltf_load_buffer_base64(&options, size, encoded.data(), &decoded),
          "image base64 decode");
    std::unique_ptr<void, decltype(&std::free)> owner(decoded, &std::free);
    bytes.resize(size);
    std::memcpy(bytes.data(), decoded, size);
  } else {
    if (!source->uri)
      invalid("image has no URI or bufferView.");
    std::string uri = source->uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    auto resolved = (path.parent_path() / uri).lexically_normal();
    if (uri.empty() || !std::filesystem::is_regular_file(resolved))
      invalid("image file not found: " + resolved.string());
    imagePath = resolved.string();
  }
}
Material material(cgltf_material const &m, std::filesystem::path const &path) {
  Material result;
  result.name = m.name ? m.name : "";
  auto const &pbr = m.pbr_metallic_roughness;
  std::copy_n(pbr.base_color_factor, 4, &result.tint.x);
  result.metallicFactor = pbr.metallic_factor;
  result.roughnessFactor = pbr.roughness_factor;
  std::copy_n(m.emissive_factor, 3, &result.emissiveFactor.x);
  result.normalScale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
  result.occlusionStrength =
      m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
  result.alphaCutoff = m.alpha_cutoff;
  result.doubleSided = m.double_sided;
  switch (m.alpha_mode) {
  case cgltf_alpha_mode_opaque:
    result.alphaMode = AlphaMode::Opaque;
    break;
  case cgltf_alpha_mode_mask:
    result.alphaMode = AlphaMode::Mask;
    break;
  case cgltf_alpha_mode_blend:
    result.alphaMode = AlphaMode::Blend;
    break;
  default:
    invalid("invalid alpha mode.");
  }
  auto bind = [&](cgltf_texture_view const &view, std::string &file,
                  std::vector<std::byte> &bytes,
                  TextureSamplerDescription &description, int &set) {
    image(view, path, file, bytes);
    description = sampler(view.texture ? view.texture->sampler : nullptr);
    set = texCoord(view);
  };
  bind(pbr.base_color_texture, result.albedoPath, result.albedoBytes,
       result.albedoSampler, result.albedoTexCoord);
  bind(pbr.metallic_roughness_texture, result.metallicRoughnessPath,
       result.metallicRoughnessBytes, result.metallicRoughnessSampler,
       result.metallicRoughnessTexCoord);
  bind(m.normal_texture, result.normalPath, result.normalBytes,
       result.normalSampler, result.normalTexCoord);
  bind(m.occlusion_texture, result.occlusionPath, result.occlusionBytes,
       result.occlusionSampler, result.occlusionTexCoord);
  bind(m.emissive_texture, result.emissivePath, result.emissiveBytes,
       result.emissiveSampler, result.emissiveTexCoord);
  if (m.has_specular) {
    if (m.unlit || m.has_pbr_specular_glossiness)
      invalid("KHR_materials_specular cannot coexist with "
              "unlit/specular-glossiness.");
    result.specularFactor = m.specular.specular_factor;
    std::copy_n(m.specular.specular_color_factor, 3,
                &result.specularColorFactor.x);
    if (!std::isfinite(result.specularFactor) || result.specularFactor < 0 ||
        result.specularFactor > 1)
      invalid(
          "KHR_materials_specular specularFactor must be finite and in [0,1].");
    for (unsigned c = 0; c < 3; ++c)
      if (!std::isfinite(result.specularColorFactor[c]) ||
          result.specularColorFactor[c] < 0)
        invalid("KHR_materials_specular specularColorFactor must be finite and "
                "nonnegative.");
    bind(m.specular.specular_texture, result.specularPath, result.specularBytes,
         result.specularSampler, result.specularTexCoord);
    bind(m.specular.specular_color_texture, result.specularColorPath,
         result.specularColorBytes, result.specularColorSampler,
         result.specularColorTexCoord);
  }
  return result;
}
Aabb emptyBounds() {
  return {.min = glm::vec3{std::numeric_limits<float>::max()},
          .max = glm::vec3{std::numeric_limits<float>::lowest()},
          .valid = false};
}
void includePoint(Aabb &bounds, glm::vec3 point) {
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
  bounds.valid = true;
}
Aabb transformBounds(Aabb const &local, glm::mat4 const &matrix) {
  Aabb world = emptyBounds();
  for (int i = 0; i < 8; ++i)
    includePoint(
        world,
        glm::vec3(matrix * glm::vec4{(i & 1) ? local.max.x : local.min.x,
                                     (i & 2) ? local.max.y : local.min.y,
                                     (i & 4) ? local.max.z : local.min.z, 1}));
  return world;
}
cgltf_accessor const *attribute(cgltf_primitive const &p,
                                cgltf_attribute_type type, int set = 0) {
  cgltf_accessor const *result = nullptr;
  for (std::size_t i = 0; i < p.attributes_count; ++i)
    if (p.attributes[i].type == type && p.attributes[i].index == set) {
      if (result)
        invalid("duplicate vertex attribute.");
      result = p.attributes[i].data;
    }
  return result;
}
Mesh primitive(cgltf_primitive const &p) {
  if (p.type != cgltf_primitive_type_triangles)
    invalid("only triangle primitives are supported.");
  if (p.has_draco_mesh_compression)
    invalid("KHR_draco_mesh_compression decoding is not implemented.");
  if (p.targets_count)
    invalid("morph targets are outside the static-scene importer.");
  auto const *position = attribute(p, cgltf_attribute_type_position);
  if (!position)
    invalid("primitive has no POSITION.");
  auto positions =
      unpack(*position, cgltf_type_vec3, position->count, "POSITION");
  auto const *normal = attribute(p, cgltf_attribute_type_normal);
  auto const *tangent = attribute(p, cgltf_attribute_type_tangent);
  auto normals =
      normal ? unpack(*normal, cgltf_type_vec3, position->count, "NORMAL")
             : std::vector<float>{};
  auto tangents =
      tangent ? unpack(*tangent, cgltf_type_vec4, position->count, "TANGENT")
              : std::vector<float>{};
  auto const *color = attribute(p, cgltf_attribute_type_color);
  if (color && color->type != cgltf_type_vec3 && color->type != cgltf_type_vec4)
    invalid("COLOR_0 must be VEC3/VEC4.");
  auto colors = color ? unpack(*color, color->type, position->count, "COLOR_0")
                      : std::vector<float>{};
  std::unordered_map<int, std::vector<float>> uvSets;
  auto uv = [&](cgltf_texture_view const &view) {
    int const set = texCoord(view);
    if (!uvSets.contains(set)) {
      auto const *accessor = attribute(p, cgltf_attribute_type_texcoord, set);
      if (!accessor && view.texture)
        invalid("material requires missing TEXCOORD_" + std::to_string(set));
      uvSets.emplace(set, accessor
                              ? unpack(*accessor, cgltf_type_vec2,
                                       position->count, "TEXCOORD")
                              : std::vector<float>(position->count * 2, 0));
    }
    // A cached absent set must still be rejected if another texture uses it.
    if (view.texture && !attribute(p, cgltf_attribute_type_texcoord, set))
      invalid("material requires missing TEXCOORD_" + std::to_string(set));
    auto result = uvSets.at(set);
    if (view.has_transform) {
      auto const &t = view.transform;
      float const c = std::cos(t.rotation), s = std::sin(t.rotation);
      for (std::size_t i = 0; i < position->count; ++i) {
        float x = result[i * 2] * t.scale[0],
              y = result[i * 2 + 1] * t.scale[1];
        result[i * 2] = t.offset[0] + c * x - s * y;
        result[i * 2 + 1] = t.offset[1] + s * x + c * y;
      }
    }
    return result;
  };
  cgltf_material
      defaults{}; // Only texture views are used for a material-less primitive.
  auto const &m = p.material ? *p.material : defaults;
  auto baseUv = uv(m.pbr_metallic_roughness.base_color_texture);
  auto normalUv = uv(m.normal_texture);
  auto packedUv = uv(m.pbr_metallic_roughness.metallic_roughness_texture);
  auto aoUv = uv(m.occlusion_texture), emissiveUv = uv(m.emissive_texture);
  auto specularUv = uv(m.specular.specular_texture);
  auto specularColorUv = uv(m.specular.specular_color_texture);
  Mesh mesh;
  mesh.localBounds = emptyBounds();
  mesh.vertices.resize(position->count);
  for (std::size_t i = 0; i < position->count; ++i) {
    auto &v = mesh.vertices[i];
    v.position = {positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]};
    v.normal = normal ? glm::vec3{normals[3 * i], normals[3 * i + 1],
                                  normals[3 * i + 2]}
                      : glm::vec3{0, 0, 1};
    if (normal && glm::length(v.normal) < 1e-6f)
      invalid("zero vertex normal.");
    v.color = {1, 1, 1};
    if (color) {
      auto const c = cgltf_num_components(color->type);
      v.color = {colors[c * i], colors[c * i + 1], colors[c * i + 2]};
      v.alpha = c == 4 ? colors[c * i + 3] : 1.0f;
      for (std::size_t channel = 0; channel < c; ++channel)
        if (colors[c * i + channel] < 0 || colors[c * i + channel] > 1)
          invalid("vertex color components must be in [0,1].");
    }
    if (tangent) {
      v.tangent = {tangents[4 * i], tangents[4 * i + 1], tangents[4 * i + 2],
                   tangents[4 * i + 3]};
      if (std::abs(v.tangent.w) != 1.0f ||
          glm::length(glm::vec3{v.tangent}) < 1e-6f)
        invalid("invalid authored tangent.");
    }
    v.uv = {baseUv[2 * i], baseUv[2 * i + 1]};
    v.normalUv = {normalUv[2 * i], normalUv[2 * i + 1]};
    v.metallicRoughnessUv = {packedUv[2 * i], packedUv[2 * i + 1]};
    v.occlusionUv = {aoUv[2 * i], aoUv[2 * i + 1]};
    v.emissiveUv = {emissiveUv[2 * i], emissiveUv[2 * i + 1]};
    v.specularUv = {specularUv[2 * i], specularUv[2 * i + 1]};
    v.specularColorUv = {specularColorUv[2 * i], specularColorUv[2 * i + 1]};
    includePoint(mesh.localBounds, v.position);
  }
  if (p.indices)
    mesh.indices = unpackIndices(*p.indices);
  else {
    mesh.indices.resize(mesh.vertices.size());
    std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
  }
  if (mesh.indices.size() % 3)
    invalid("triangle index count is not divisible by three.");
  for (auto index : mesh.indices)
    if (index >= mesh.vertices.size())
      invalid("triangle index exceeds POSITION count.");
  if (!normal) {
    // glTF requires flat normals when NORMAL is absent: split shared vertices.
    std::vector<Vertex> flat;
    flat.reserve(mesh.indices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
      auto a = mesh.vertices[mesh.indices[i]],
           b = mesh.vertices[mesh.indices[i + 1]],
           c = mesh.vertices[mesh.indices[i + 2]];
      auto n = glm::cross(b.position - a.position, c.position - a.position);
      n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3{0, 0, 1};
      a.normal = b.normal = c.normal = n;
      flat.insert(flat.end(), {a, b, c});
    }
    mesh.vertices = std::move(flat);
    std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
  }
  // Authored tangent basis is defined for TEXCOORD_0. A different/transformed
  // normal UV requires a basis regenerated from the actual baked coordinates.
  if (!tangent || !normal ||
      (m.normal_texture.texture &&
       (texCoord(m.normal_texture) != 0 || m.normal_texture.has_transform)))
    generateMeshTangents(mesh, true);
  return mesh;
}
PunctualLight importLight(cgltf_light const &source) {
  PunctualLight light;
  light.name = source.name ? source.name : "glTF light";
  switch (source.type) {
  case cgltf_light_type_directional:
    light.type = PunctualLightType::Directional;
    break;
  case cgltf_light_type_point:
    light.type = PunctualLightType::Point;
    break;
  case cgltf_light_type_spot:
    light.type = PunctualLightType::Spot;
    break;
  default:
    invalid("unknown punctual light type");
  }
  if (source.has_range && (!std::isfinite(source.range) || source.range <= 0))
    invalid("punctual range must be positive when present");
  if (light.type == PunctualLightType::Spot && !source.has_spot)
    invalid("spot light requires spot object");
  light.color = {source.color[0], source.color[1], source.color[2]};
  light.intensity = source.intensity;
  light.range = source.range;
  light.innerCone = source.spot_inner_cone_angle;
  light.outerCone = source.spot_outer_cone_angle;
  try {
    validatePunctualLight(light);
  } catch (std::runtime_error const &error) {
    invalid(error.what());
  }
  return light;
}

ImportedScene load(std::filesystem::path const &path,
                   cgltf_file_type expected) {
  cgltf_options options{};
  cgltf_data *raw = nullptr;
  auto const parsed = cgltf_parse_file(&options, path.string().c_str(), &raw);
  std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, &cgltf_free);
  check(parsed, "parse");
  if (data->file_type != expected)
    invalid("file content does not match the requested glTF/GLB format.");
  auto supported = [](std::string_view extension) {
    return extension == "KHR_texture_transform" ||
           extension == "KHR_mesh_quantization" ||
           extension == "KHR_materials_specular" ||
           extension == "KHR_lights_punctual";
  };
  ImportedScene result;
  for (std::size_t i = 0; i < data->extensions_required_count; ++i)
    if (!supported(data->extensions_required[i]))
      invalid("unsupported required extension: " +
              std::string(data->extensions_required[i]));
  for (std::size_t i = 0; i < data->extensions_used_count; ++i)
    if (!supported(data->extensions_used[i]))
      result.warnings.push_back("Unsupported optional glTF extension " +
                                std::string(data->extensions_used[i]) +
                                "; using core fallback.");
  check(cgltf_load_buffers(&options, data.get(), path.string().c_str()),
        "load buffers");
  validateStorage(*data);
  check(cgltf_validate(data.get()), "validate");
  for (std::size_t i = 0; i < data->lights_count; ++i)
    (void)importLight(data->lights[i]);
  for (std::size_t i = 0; i < data->materials_count; ++i)
    result.materials.push_back(material(data->materials[i], path));
  std::optional<MaterialId> defaultId;
  auto defaultMaterial = [&] {
    if (!defaultId) {
      defaultId = static_cast<MaterialId>(result.materials.size());
      Material m;
      m.name = "glTF default";
      m.metallicFactor = 1;
      result.materials.push_back(std::move(m));
    }
    return *defaultId;
  };
  struct Ref {
    MeshId mesh;
    MaterialId material;
  };
  std::vector<std::vector<Ref>> refs(data->meshes_count);
  for (std::size_t i = 0; i < data->meshes_count; ++i)
    for (std::size_t j = 0; j < data->meshes[i].primitives_count; ++j) {
      auto const &p = data->meshes[i].primitives[j];
      MaterialId id =
          p.material ? static_cast<MaterialId>(p.material - data->materials)
                     : defaultMaterial();
      refs[i].push_back({static_cast<MeshId>(result.meshes.size()), id});
      result.meshes.push_back(primitive(p));
    }
  struct Visit {
    cgltf_node const *node;
    glm::mat4 parent;
  };
  std::vector<Visit> pending;
  auto const *scene = data->scene          ? data->scene
                      : data->scenes_count ? &data->scenes[0]
                                           : nullptr;
  if (scene) {
    for (std::size_t i = scene->nodes_count; i > 0; --i)
      pending.push_back({scene->nodes[i - 1], glm::mat4{1}});
  } else {
    for (std::size_t i = data->nodes_count; i > 0; --i)
      if (!data->nodes[i - 1].parent)
        pending.push_back({&data->nodes[i - 1], glm::mat4{1}});
  }
  std::unordered_set<cgltf_node const *> visited;
  while (!pending.empty()) {
    auto const visit = pending.back();
    pending.pop_back();
    if (!visited.insert(visit.node).second)
      invalid("node is referenced more than once in the selected scene.");
    auto const &node = *visit.node;
    if (node.skin || node.has_mesh_gpu_instancing)
      invalid("skinning/instancing requires an importer outside this "
              "static-scene path.");
    glm::mat4 local{1};
    cgltf_node_transform_local(&node, &local[0][0]);
    auto const world = visit.parent * local;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        if (!std::isfinite(world[c][r]))
          invalid("non-finite node transform.");
    if (node.light) {
      auto const &source = *node.light;
      auto light = importLight(source);
      if (!source.name && node.name)
        light.name = node.name;
      light.position = glm::vec3(world * glm::vec4(0, 0, 0, 1));
      light.direction = glm::vec3(world * glm::vec4(0, 0, -1, 0));
      try {
        validatePunctualLight(light);
      } catch (std::runtime_error const &error) {
        invalid(error.what());
      }
      if (light.type != PunctualLightType::Point)
        light.direction =
            glm::vec3(glm::normalize(glm::dvec3(light.direction)));
      result.lights.push_back(std::move(light));
    }
    if (node.mesh)
      for (auto const &ref : refs.at(node.mesh - data->meshes)) {
        SceneObject object;
        object.meshId = ref.mesh;
        object.materialId = ref.material;
        object.transform.importedMatrix = world;
        object.worldBounds =
            transformBounds(result.meshes.at(ref.mesh).localBounds, world);
        result.objects.push_back(object);
      }
    for (std::size_t i = node.children_count; i > 0; --i)
      pending.push_back({node.children[i - 1], world});
  }
  if (result.meshes.empty() || result.objects.empty())
    invalid("import produced no drawable objects.");
  return result;
}
} // namespace

ImportedScene loadStaticGltfScene(std::filesystem::path const &path,
                                  std::string) {
  return load(path, cgltf_file_type_gltf);
}
ImportedScene loadStaticGlbScene(std::filesystem::path const &path,
                                 std::string) {
  return load(path, cgltf_file_type_glb);
}
