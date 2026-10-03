#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <string_view>

// Counts engine-owned objects, unique backing memory and suballocations, not driver heap
// residency. Shared objects own one lease regardless of how many bindings exist.
class ResourceLedger {
  struct State;
  struct Owner;
public:
  enum class Domain { Persistent, PreparedScene, LiveScene, RetiredScene,
                      SharedTextures, Staging, AllocatorBlocks, Count };
  static constexpr std::size_t domainCount = static_cast<std::size_t>(Domain::Count);
  static std::string_view name(Domain domain);
  struct Footprint {
    std::uint64_t allocations = 0, buffers = 0, images = 0, imageViews = 0,
                  samplers = 0, descriptorPools = 0, descriptorSets = 0;
    std::uint64_t payloadBytes = 0, allocatedBytes = 0,
                  deviceLocalBytes = 0, hostVisibleBytes = 0;
    // Payload belongs to resources. Backing bytes count unique allocator blocks; suballocation bytes never add to allocatedBytes.
    std::uint64_t suballocations = 0, suballocatedBytes = 0,
                  suballocationDeviceLocalBytes = 0, suballocationHostVisibleBytes = 0;
    bool operator==(Footprint const &) const = default;
  };
  struct Snapshot {
    Footprint current, peak;
    std::array<Footprint, domainCount> domains{}, domainPeaks{};
    Footprint const &at(Domain domain) const {
      return domains.at(static_cast<std::size_t>(domain));
    }
  };
  class Scope;
  class Lease {
    friend class Scope;
    Lease(std::shared_ptr<Owner> owner, Footprint footprint);
    std::shared_ptr<Owner> owner_;
    Footprint footprint_;
  public:
    Lease() = default;
    ~Lease();
    Lease(Lease const &) = delete;
    Lease &operator=(Lease const &) = delete;
    Lease(Lease &&) noexcept;
    Lease &operator=(Lease &&) noexcept;
    void reset() noexcept;
  };
  class Scope {
    friend class ResourceLedger;
    explicit Scope(std::shared_ptr<Owner> owner) : owner_(std::move(owner)) {}
    std::shared_ptr<Owner> owner_;
  public:
    Scope() = default;
    explicit operator bool() const { return bool(owner_); }
    Lease track(Footprint footprint) const;
    // Move all private resources atomically; allocation totals do not change.
    void setDomain(Domain domain) const noexcept;
  };
  ResourceLedger();
  Scope scope(Domain domain) const;
  Snapshot snapshot() const;
private:
  std::shared_ptr<State> state_;
};
