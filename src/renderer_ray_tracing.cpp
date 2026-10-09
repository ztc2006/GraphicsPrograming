#include "renderer.hpp"
#include <stdexcept>

void Renderer::setRenderMethod(RenderMethod method) {
  if (activeFrame_)
    throw std::runtime_error("Cannot switch renderer while recording a frame");
  if (method != RenderMethod::Raster && method != RenderMethod::RayTracing)
    throw std::runtime_error("Unknown rendering method");
  if (method == RenderMethod::RayTracing && !rayTracingAvailable())
    throw std::runtime_error("Ray tracing unavailable: " +
                             rayTracingUnavailableReason());
  if (method != renderMethod_) {
    renderMethod_ = method;
    invalidateTemporalHistory();
  }
}
std::vector<vk::DescriptorImageInfo>
Renderer::rtTextures(std::vector<glm::uvec4> *main,
                     std::vector<glm::uvec4> *more,
                     std::vector<glm::uvec4> *maps,
                     MaterialGpuStore const *store, std::size_t count) const {
  if (!store) {
    store = &materials();
    count = sceneAssets_->materialCount_;
  }
  std::vector<vk::DescriptorImageInfo> textures;
  auto add = [&](TextureResources const *t) {
    if (!t)
      return 0xffffffffu;
    for (unsigned i = 0; i < textures.size(); ++i)
      if (textures[i].imageView == t->imageView() &&
          textures[i].sampler == t->sampler())
        return i;
    textures.push_back({t->sampler(), t->imageView(),
                        vk::ImageLayout::eShaderReadOnlyOptimal});
    return unsigned(textures.size() - 1);
  };
  for (MaterialId id = 0; id < count; ++id) {
    auto const &m = store->material(id);
    glm::uvec4 t{add(&m.baseAlbedoTexture), add(&m.baseAlphaTexture),
                 unsigned(m.alphaTexture.has_value()),
                 add(m.normalTexture ? &*m.normalTexture : nullptr)};
    glm::uvec4 u{add(m.metallicRoughnessTexture ? &*m.metallicRoughnessTexture
                                                : nullptr),
                 add(m.occlusionTexture ? &*m.occlusionTexture : nullptr),
                 add(m.emissiveTexture ? &*m.emissiveTexture : nullptr),
                 add(m.specularTexture ? &*m.specularTexture : nullptr)};
    glm::uvec4 v{
        add(m.specularColorTexture ? &*m.specularColorTexture : nullptr),
        0xffffffffu, 0xffffffffu, 0};
    if (main)
      main->push_back(t);
    if (more)
      more->push_back(u);
    if (maps)
      maps->push_back(v);
  }
  return textures;
}
void Renderer::prepareRayTracing(SceneDrawList const &scene) {
  if (!sceneAssets_ || !sceneAssets_->rtGeometry_ || !rayTracing_)
    throw std::runtime_error("No committed RT scene");
  std::vector<glm::uvec4> main, more, maps;
  auto textures = rtTextures(&main, &more, &maps);
  std::vector<RtInstanceInput> input;
  for (auto list : {scene.allOpaque.empty() ? scene.opaque : scene.allOpaque,
                    scene.allMask.empty() ? scene.mask : scene.allMask,
                    scene.allTransparent.empty() ? scene.transparent
                                                 : scene.allTransparent})
    for (auto const &item : list) {
      validateDrawItem(item, false);
      auto const &m = materials().material(item.materialId);
      MaterialGpuStore::MaterialUniformBufferObject factors;
      m.uniform.read(std::as_writable_bytes(std::span{&factors, 1}));
      auto alpha = m.alphaParams;
      alpha.z = m.alphaTexture.has_value() ? 1.f : 0.f;
      RtInstanceInput v{item.meshId, item.materialId, item.modelMatrix,
                        m.tint,      alpha,           m.doubleSided};
      v.primaryVisible = item.primaryVisible;
      v.shadowCaster = item.shadowCaster && (m.optical.enabled || m.alphaMode != AlphaMode::Blend);
      v.sourceAreaLight = m.sourceAreaLight;
      v.optical=factors.optical;v.absorptionThickness=factors.absorptionThickness;
      v.optical.z=m.optical.solid && sceneAssets_->meshes_[item.meshId].dielectric.solid ? 1.f : 0.f;
      v.pbr = factors.pbrParams;
      v.emission = factors.emissiveFactor;
      v.specular = factors.specularColorAndWeight;
      v.normalScale = m.surfaceParams.x;
      v.albedoTexture = main[item.materialId].x;
      v.alphaTexture = main[item.materialId].y;
      v.normalTexture = main[item.materialId].w;
      v.moreTextures = more[item.materialId];
      v.maps = maps[item.materialId];
      input.push_back(v);
    }
  std::vector<RtAreaLight> areas;
  double sum = 0;
  for (unsigned i = 0; i < input.size(); ++i) {
    auto &v = input[i];
    auto e = glm::vec3(v.emission);
    auto power = glm::dot(e, glm::vec3(.2126f, .7152f, .0722f));
    if (power <= 0)
      continue;
    v.maps.z = unsigned(areas.size());
    auto count = sceneAssets_->rtGeometry_->mesh(v.mesh).indexCount / 3;
    v.maps.w = count;
    for (unsigned primitive = 0; primitive < count; ++primitive) {
      auto p = sceneAssets_->rtGeometry_->triangle(v.mesh, primitive);
      glm::vec3 p0 = v.transform * glm::vec4(p[0], 1),
                p1 = v.transform * glm::vec4(p[1], 1),
                p2 = v.transform * glm::vec4(p[2], 1);
      auto area = .5f * glm::length(glm::cross(p1 - p0, p2 - p0));
      auto n = glm::transpose(glm::inverse(glm::mat3(v.transform))) *
               glm::cross(p[1] - p[0], p[2] - p[0]);
      if (glm::dot(n, n) > 0)
        n = glm::normalize(n);
      else
        n = {0, 0, 1};
      auto weight = double(area) * power;
      sum += weight;
      areas.push_back({glm::vec4(p0, area),
                       glm::vec4(p1, float(weight)),
                       glm::vec4(p2, float(sum)),
                       glm::vec4(n, 0),
                       {i, primitive, unsigned(v.doubleSided), 0}});
    }
  }
  if (sum > 0)
    for (auto &a : areas) {
      a.p1Probability.w = float(a.p1Probability.w / sum);
      a.p2Cdf.w = float(a.p2Cdf.w / sum);
    }
  auto packed = packPunctualLights(
      rtLighting_.punctualLights,
      device_.physicalDevice().getProperties().limits.maxStorageBufferRange);
  unsigned j = 0;
  for (auto const &light : rtLighting_.punctualLights)
    if (light.enabled)
      packed.lights[j++].cones.w = light.castsShadow ? 1.f : 0.f;
  RtLightingFrame frame;
  frame.transport = rtPush_.options.x == 3;
  auto dir = rtLighting_.direction;
  if (glm::length(dir) > 0)
    dir = glm::normalize(dir);
  else
    dir = {0, 1, 0};
  frame.settings.sunDirection = glm::vec4(dir, rtLighting_.intensity);
  frame.settings.sunColor =
      glm::vec4(rtLighting_.color, float(rtLighting_.sunEnabled));
  frame.settings.strengths = {rtLighting_.diffuseStrength,
                              rtLighting_.specularStrength,
                              rtLighting_.environmentDiffuseStrength,
                              rtLighting_.environmentSpecularStrength};
  frame.settings.environment = {rtLighting_.environmentIntensity,
                                rtLighting_.environmentRotation,
                                float(rtShadows_),
                                float((normalMapsEnabled_ ? 1 : 0) |
                                      (rtLighting_.specularAaEnabled ? 2 : 0))};
  frame.settings.counts.w = rtMaxBounces_;
  frame.settings.sampling.x = rtSpp_;
  frame.settings.sampling.z = rtAccumulate_;
  frame.punctual = packed.lights;
  frame.areas = areas;
  frame.environment = {environmentTexture_.sampler(),
                       environmentTexture_.imageView(),
                       vk::ImageLayout::eShaderReadOnlyOptimal};
  auto append = [&](auto const &value) {
    auto bytes = std::as_bytes(std::span{&value, 1});
    frame.historyKey.insert(frame.historyKey.end(), bytes.begin(), bytes.end());
  };
  append(rtPush_.inverseViewProjection);
  append(rtPush_.camera);
  append(rtPush_.options.x);
  append(rtPush_.options.y);
  append(rtPush_.options.z);
  rayTracing_->prepare(activeFrame_->frameIndex, *sceneAssets_->rtGeometry_,
                       input, textures, frame);
}
Renderer::FrameGraph
Renderer::buildRayTracingGraph(std::uint32_t imageIndex) const {
  using G = RenderGraph;
  G graph;
  FrameGraph frame;
  frame.rayTracing = true;
  frame.hdr = rayTracing_->importTarget(graph);
  frame.output = graph.importImage(
      {"Swapchain output", swapChain_->images()[imageIndex],
       *swapChain_->imageViews()[imageIndex], swapChain_->imageFormat(),
       swapChain_->extent(), vk::ImageAspectFlagBits::eColor,
       vk::ImageUsageFlagBits::eColorAttachment, true,
       imageStates_.output[imageIndex]});
  frame.rtBuildPass = graph.addPass("RT TLAS build (frame slot)", {});
  frame.mainPass = graph.addPass(
      "RT path tracing (primary + transport)",
      {{frame.hdr,
        rayTracing_->accumulatedSamples() ? G::Usage::RayTracingReadWrite
                                          : G::Usage::RayTracingWrite,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, true}});
  graph.dependsOn(frame.mainPass, *frame.rtBuildPass);
  frame.outputPass = graph.addPass(
      "Display output (exposure + filmic + sRGB)",
      {{frame.hdr, G::Usage::SampledColor},
       {frame.output, G::Usage::ColorAttachment,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore, true}});
  if (uiDrawCallback_)
    frame.uiPass = graph.addPass(
        "UI", {{frame.output, G::Usage::ColorAttachment,
                vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore}});
  graph.exportImage(frame.output, G::Usage::Present);
  frame.plan = graph.compile();
  return frame;
}
