#include "resource_ledger.hpp"
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, char const *message) {
  if (!condition) throw std::runtime_error(message);
}
using Domain = ResourceLedger::Domain;
void exercise() {
  ResourceLedger ledger;
  auto persistent = ledger.scope(Domain::Persistent);
  auto base = persistent.track({.allocations = 1, .buffers = 1,
                                .payloadBytes = 32, .allocatedBytes = 64});
  auto baseline = ledger.snapshot().current;
  {
    auto oldScene = ledger.scope(Domain::LiveScene);
    auto newScene = ledger.scope(Domain::PreparedScene);
    auto staging = ledger.scope(Domain::Staging);
    auto shared = ledger.scope(Domain::SharedTextures);
    auto oldBuffer = oldScene.track({.allocations = 1, .buffers = 1,
                                     .payloadBytes = 100, .allocatedBytes = 128});
    auto candidateBuffer = newScene.track({.allocations = 1, .buffers = 1,
                                           .payloadBytes = 100, .allocatedBytes = 128});
    auto copy = staging.track({.allocations = 1, .buffers = 1,
                              .payloadBytes = 100, .allocatedBytes = 128});
    auto texture = std::make_shared<ResourceLedger::Lease>(shared.track(
        {.allocations = 1, .images = 1, .payloadBytes = 4, .allocatedBytes = 256}));
    auto alias = texture;
    auto before = ledger.snapshot();
    require(before.current.allocatedBytes == 704 && before.current.images == 1,
            "Coexistence accounting double-counted shared bindings");
    oldScene.setDomain(Domain::RetiredScene);
    newScene.setDomain(Domain::LiveScene);
    auto committed = ledger.snapshot();
    require(committed.current == before.current &&
                committed.at(Domain::RetiredScene).allocatedBytes == 128 &&
                committed.at(Domain::PreparedScene).allocatedBytes == 0,
            "Commit changed allocation totals or left private resources misclassified");
    oldBuffer.reset();
    copy.reset();
    texture.reset();
    require(ledger.snapshot().current.images == 1, "Alias lost physical image lifetime");
    alias.reset();
    require(ledger.snapshot().current.images == 0 &&
                ledger.snapshot().peak.allocatedBytes == 704,
            "Reclamation changed historical coexistence peak");
    ResourceLedger::Lease moved = std::move(candidateBuffer);
    auto replacement = newScene.track({.buffers = 1});
    replacement = std::move(moved);
    require(ledger.snapshot().current.buffers == 2, "Lease move assignment leaked accounting");
  }
  require(ledger.snapshot().current == baseline, "Rollback did not restore baseline");
  {
    auto scene = ledger.scope(Domain::PreparedScene);
    std::latch allocated(2), release(1);
    auto worker = [&] {
      std::vector<ResourceLedger::Lease> leases;
      for (int i = 0; i < 100; ++i)
        leases.push_back(scene.track({.allocations = 1, .buffers = 1,
                                      .payloadBytes = 32, .allocatedBytes = 64}));
      allocated.count_down();
      release.wait();
    };
    auto first = std::async(std::launch::async, worker);
    auto second = std::async(std::launch::async, worker);
    allocated.wait();
    scene.setDomain(Domain::LiveScene);
    auto concurrent = ledger.snapshot();
    // Always release test workers before an assertion can unwind their futures.
    release.count_down();
    first.get(); second.get();
    require(concurrent.at(Domain::LiveScene).buffers == 200 &&
                concurrent.current.allocatedBytes == 12864,
            "Concurrent allocation/commit snapshots were inconsistent");
  }
  require(ledger.snapshot().current == baseline, "Concurrent release leaked counters");
  {
    auto backing = ledger.scope(Domain::AllocatorBlocks).track(
        {.allocations = 1, .allocatedBytes = 4096, .hostVisibleBytes = 4096});
    auto prepared = ledger.scope(Domain::PreparedScene);
    auto a = prepared.track({.buffers = 1, .payloadBytes = 7,
                             .suballocations = 1, .suballocatedBytes = 64});
    auto b = ledger.scope(Domain::LiveScene).track(
        {.buffers = 1, .payloadBytes = 11, .suballocations = 1, .suballocatedBytes = 32});
    auto physical = ledger.snapshot();
    require(physical.current.allocatedBytes == baseline.allocatedBytes + 4096 &&
                physical.current.suballocatedBytes == 96 && physical.current.suballocations == 2,
            "Shared backing and allocation ranges were added together");
    prepared.setDomain(Domain::LiveScene);
    require(ledger.snapshot().current == physical.current &&
                ledger.snapshot().at(Domain::LiveScene).suballocatedBytes == 96 &&
                ledger.snapshot().at(Domain::AllocatorBlocks).allocatedBytes == 4096,
            "Suballocation transfer incorrectly moved its shared block");
  }
  require(ledger.snapshot().current == baseline && ledger.snapshot().peak.suballocatedBytes >= 96,
          "Suballocation release or peak failed");
  base.reset();
  require(ledger.snapshot().current == ResourceLedger::Footprint{}, "Ledger did not return to zero");
}
}
int main() {
  try {
    exercise();
    std::cout << "PASS resource ledger: coexistence, alias lifetime, rollback, concurrent preparation\n";
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
