#include "resource_ledger.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {
using F = ResourceLedger::Footprint;
constexpr auto fields = std::to_array<std::uint64_t F::*>({
    &F::allocations, &F::buffers, &F::images, &F::imageViews, &F::samplers,
    &F::descriptorPools, &F::descriptorSets, &F::payloadBytes,
    &F::allocatedBytes, &F::deviceLocalBytes, &F::hostVisibleBytes,
    &F::suballocations, &F::suballocatedBytes, &F::suballocationDeviceLocalBytes, &F::suballocationHostVisibleBytes});
void add(F &a, F const &b) {
  for (auto field : fields) a.*field += b.*field;
}
void subtract(F &a, F const &b) {
  for (auto field : fields) {
    assert(a.*field >= b.*field);
    a.*field -= b.*field;
  }
}
void peak(F &a, F const &b) {
  for (auto field : fields) a.*field = std::max(a.*field, b.*field);
}
}
struct ResourceLedger::State {
  std::mutex mutex;
  Snapshot counters;
};
struct ResourceLedger::Owner {
  std::shared_ptr<State> state;
  Domain domain;
  Footprint current;
};
std::string_view ResourceLedger::name(Domain domain) {
  static constexpr std::array names{"persistent", "prepared_scene", "live_scene",
                                    "retired_scene", "shared_textures", "staging", "allocator_blocks"};
  return names.at(static_cast<std::size_t>(domain));
}
ResourceLedger::ResourceLedger() : state_(std::make_shared<State>()) {}
ResourceLedger::Scope ResourceLedger::scope(Domain domain) const {
  if (static_cast<std::size_t>(domain) >= domainCount)
    throw std::invalid_argument("Invalid resource ledger domain");
  return Scope(std::make_shared<Owner>(Owner{state_, domain, {}}));
}
ResourceLedger::Snapshot ResourceLedger::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return state_->counters;
}
ResourceLedger::Lease ResourceLedger::Scope::track(Footprint footprint) const {
  if (!owner_) throw std::logic_error("Cannot track without a resource scope");
  return Lease(owner_, footprint);
}
ResourceLedger::Lease::Lease(std::shared_ptr<Owner> owner, Footprint footprint)
    : owner_(std::move(owner)), footprint_(footprint) {
  auto &state = *owner_->state;
  std::lock_guard lock(state.mutex);
  auto index = static_cast<std::size_t>(owner_->domain);
  add(owner_->current, footprint_);
  add(state.counters.domains[index], footprint_);
  add(state.counters.current, footprint_);
  peak(state.counters.domainPeaks[index], state.counters.domains[index]);
  peak(state.counters.peak, state.counters.current);
}
ResourceLedger::Lease::~Lease() { reset(); }
ResourceLedger::Lease::Lease(Lease &&other) noexcept
    : owner_(std::move(other.owner_)), footprint_(other.footprint_) {}
ResourceLedger::Lease &ResourceLedger::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::move(other.owner_);
    footprint_ = other.footprint_;
  }
  return *this;
}
void ResourceLedger::Lease::reset() noexcept {
  if (!owner_) return;
  {
    auto &state = *owner_->state;
    std::lock_guard lock(state.mutex);
    subtract(owner_->current, footprint_);
    subtract(state.counters.domains[static_cast<std::size_t>(owner_->domain)], footprint_);
    subtract(state.counters.current, footprint_);
  }
  owner_.reset();
}
void ResourceLedger::Scope::setDomain(Domain domain) const noexcept {
  assert(owner_ && static_cast<std::size_t>(domain) < domainCount);
  auto &state = *owner_->state;
  std::lock_guard lock(state.mutex);
  auto index = static_cast<std::size_t>(domain);
  subtract(state.counters.domains[static_cast<std::size_t>(owner_->domain)], owner_->current);
  add(state.counters.domains[index], owner_->current);
  owner_->domain = domain;
  peak(state.counters.domainPeaks[index], state.counters.domains[index]);
}
