#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "asset_ids.hpp"
#include "mesh.hpp"
#include "scene_object.hpp"
#include "transform.hpp"

struct Entity {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  friend bool operator==(Entity const &, Entity const &) = default;
};

class EntityRegistry {
public:
  Entity create() {
    if (!freeList_.empty()) {
      std::uint32_t const index = freeList_.back();
      freeList_.pop_back();
      alive_[index] = true;
      ++aliveCount_;
      return Entity{.index = index, .generation = generations_[index]};
    }

    std::uint32_t const index = static_cast<std::uint32_t>(generations_.size());
    generations_.push_back(0);
    alive_.push_back(true);
    ++aliveCount_;
    return Entity{.index = index, .generation = 0};
  }

  void destroy(Entity entity) {
    if (!valid(entity)) {
      return;
    }
    alive_[entity.index] = false;
    ++generations_[entity.index];
    --aliveCount_;
    freeList_.push_back(entity.index);
  }

  void clear() {
    generations_.clear();
    alive_.clear();
    freeList_.clear();
    aliveCount_ = 0;
  }

  bool valid(Entity entity) const {
    return entity.index < generations_.size() && alive_[entity.index] &&
           generations_[entity.index] == entity.generation;
  }

  std::size_t aliveCount() const { return aliveCount_; }

private:
  std::vector<std::uint32_t> generations_;
  std::vector<bool> alive_;
  std::vector<std::uint32_t> freeList_;
  std::size_t aliveCount_ = 0;
};

template <typename T> class ComponentStorage {
public:
  void clear() {
    sparse_.clear();
    denseEntities_.clear();
    denseComponents_.clear();
  }

  bool has(Entity entity) const {
    if (entity.index >= sparse_.size()) {
      return false;
    }
    std::size_t const denseIndex = sparse_[entity.index];
    return denseIndex != kInvalidIndex && denseIndex < denseEntities_.size() &&
           denseEntities_[denseIndex] == entity;
  }

  T &add(Entity entity, T component) {
    if (entity.index >= sparse_.size()) {
      sparse_.resize(entity.index + 1, kInvalidIndex);
    }

    if (has(entity)) {
      denseComponents_[sparse_[entity.index]] = std::move(component);
      return denseComponents_[sparse_[entity.index]];
    }

    std::size_t const denseIndex = denseComponents_.size();
    sparse_[entity.index] = denseIndex;
    denseEntities_.push_back(entity);
    denseComponents_.push_back(std::move(component));
    return denseComponents_.back();
  }

  T &get(Entity entity) {
    if (!has(entity)) {
      throw std::runtime_error("Entity does not have requested component.");
    }
    return denseComponents_[sparse_[entity.index]];
  }

  T const &get(Entity entity) const {
    if (!has(entity)) {
      throw std::runtime_error("Entity does not have requested component.");
    }
    return denseComponents_[sparse_[entity.index]];
  }

  void remove(Entity entity) {
    if (!has(entity)) {
      return;
    }

    std::size_t const denseIndex = sparse_[entity.index];
    std::size_t const lastIndex = denseComponents_.size() - 1;
    if (denseIndex != lastIndex) {
      denseEntities_[denseIndex] = denseEntities_[lastIndex];
      denseComponents_[denseIndex] = std::move(denseComponents_[lastIndex]);
      sparse_[denseEntities_[denseIndex].index] = denseIndex;
    }

    sparse_[entity.index] = kInvalidIndex;
    denseEntities_.pop_back();
    denseComponents_.pop_back();
  }

  std::size_t size() const { return denseComponents_.size(); }

private:
  static constexpr std::size_t kInvalidIndex =
      std::numeric_limits<std::size_t>::max();

  std::vector<std::size_t> sparse_;
  std::vector<Entity> denseEntities_;
  std::vector<T> denseComponents_;
};

struct TransformComponent {
  Transform transform{};
};

struct RenderableComponent {
  MeshId meshId = 0;
  MaterialId materialId = 0;
};

struct BoundsComponent {
  Aabb worldBounds{};
};

class SceneEcs {
public:
  void rebuildFromSceneObjects(std::vector<SceneObject> const &objects) {
    registry_.clear();
    transforms_.clear();
    renderables_.clear();
    bounds_.clear();
    sceneObjectEntities_.clear();
    sceneObjectEntities_.reserve(objects.size());

    for (SceneObject const &object : objects) {
      Entity const entity = registry_.create();
      sceneObjectEntities_.push_back(entity);
      setObjectComponents(entity, object);
    }
  }

  std::size_t entityCount() const { return registry_.aliveCount(); }
  std::size_t transformCount() const { return transforms_.size(); }
  std::size_t renderableCount() const { return renderables_.size(); }
  std::size_t boundsCount() const { return bounds_.size(); }
  std::size_t sceneObjectCount() const { return sceneObjectEntities_.size(); }

  template <typename F> void forEachSceneObject(F &&callback) {
    for (std::size_t i = 0; i < sceneObjectEntities_.size(); ++i) {
      Entity const entity = sceneObjectEntities_[i];
      callback(i, entity, transforms_.get(entity), renderables_.get(entity),
               bounds_.get(entity));
    }
  }

  template <typename F> void forEachSceneObject(F &&callback) const {
    for (std::size_t i = 0; i < sceneObjectEntities_.size(); ++i) {
      Entity const entity = sceneObjectEntities_[i];
      callback(i, entity, transforms_.get(entity), renderables_.get(entity),
               bounds_.get(entity));
    }
  }

private:
  void setObjectComponents(Entity entity, SceneObject const &object) {
    if (!registry_.valid(entity)) {
      throw std::runtime_error("Cannot assign components to invalid entity.");
    }
    transforms_.add(entity, TransformComponent{.transform = object.transform});
    renderables_.add(entity, RenderableComponent{
                                 .meshId = object.meshId,
                                 .materialId = object.materialId,
                             });
    bounds_.add(entity, BoundsComponent{.worldBounds = object.worldBounds});
  }

  EntityRegistry registry_;
  ComponentStorage<TransformComponent> transforms_;
  ComponentStorage<RenderableComponent> renderables_;
  ComponentStorage<BoundsComponent> bounds_;
  std::vector<Entity> sceneObjectEntities_;
};
