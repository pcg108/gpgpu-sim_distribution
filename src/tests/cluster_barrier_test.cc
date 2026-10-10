#include "../cluster_barrier.h"

#include <cassert>
#include <functional>
#include <iostream>

namespace {
using coord = cluster_barrier_coord;
constexpr uint32_t full = UINT32_MAX;
const coord c0(0, 0, 0), c1(1, 0, 0), t0(0, 0, 0), t1(1, 0, 0);

cluster_barrier_manager pair(unsigned threads = 32) {
  cluster_barrier_manager b;
  b.set_kernel_uid(17);
  b.register_cta(c0, t0, 0, threads, 32);
  b.register_cta(c0, t1, 1, threads, 32);
  b.seal();
  return b;
}

void rejects(const std::function<void()> &action, const char *expected) {
  bool threw = false;
  try {
    action();
  } catch (const std::runtime_error &e) {
    threw = true;
    assert(std::string(e.what()).find(expected) != std::string::npos);
  }
  assert(threw);
}

void split_phase_and_unlaunched_peer() {
  auto b = pair();
  assert(b.enabled() && b.max_ctas_per_cluster() == 2);
  b.arrive(c0, t0, 0, full, 1, 0xb80);
  assert(!b.waiting(c0, t0, 0));  // Arrival is non-blocking.
  b.wait(c0, t0, 0, full, 2, 0x19f0);
  assert(b.waiting(c0, t0, 0));  // Metadata includes the unlaunched peer.
  b.arrive(c0, t1, 0, full, 700, 0x5b30);
  assert(!b.waiting(c0, t0, 0));
  assert(b.generation(c0) == 0);  // Last arrival alone cannot reset.
  assert(!b.arrive_ready(c0, t0, 0));
  b.wait(c0, t1, 0, full, 720, 0x5b90);
  assert(b.generation(c0) == 1);
  assert(b.arrive_ready(c0, t0, 0));
}

void multiple_generations() {
  auto b = pair();
  for (unsigned gen = 0; gen != 100; ++gen) {
    auto first = gen % 2 ? t1 : t0;
    auto last = gen % 2 ? t0 : t1;
    b.arrive(c0, first, 0, full, gen * 10 + 1);
    b.arrive(c0, last, 0, full, gen * 10 + 2);
    b.wait(c0, first, 0, full, gen * 10 + 3);
    assert(!b.waiting(c0, first, 0));
    assert(!b.arrive_ready(c0, first, 0));
    assert(b.generation(c0) == gen);
    b.wait(c0, last, 0, full, gen * 10 + 4);
    assert(b.generation(c0) == gen + 1);
  }
}

void all_warps_participate() {
  auto b = pair(64);
  for (auto cta : {t0, t1}) {
    b.arrive(c0, cta, 0, full, 1);
    b.wait(c0, cta, 0, full, 2);
  }
  assert(b.waiting(c0, t0, 0));
  b.arrive(c0, t0, 1, full, 3);
  assert(b.waiting(c0, t1, 0));
  b.arrive(c0, t1, 1, full, 4);
  assert(!b.waiting(c0, t1, 0));
  b.wait(c0, t0, 1, full, 5);
  b.wait(c0, t1, 1, full, 6);
  assert(b.generation(c0) == 1);
}

void early_exit_and_partial_final_warp() {
  auto b = pair(35);
  b.exit(c0, t1, 0, full, 1);  // Warp with no barrier instructions exits.
  b.exit(c0, t1, 1, 7, 1);
  b.arrive(c0, t0, 0, full, 2);
  b.wait(c0, t0, 0, full, 3);
  assert(b.waiting(c0, t0, 0));
  assert(b.live_mask(c0, t0, 1) == 7);
  b.arrive(c0, t0, 1, 7, 4);
  assert(!b.waiting(c0, t0, 0));
  b.wait(c0, t0, 1, 7, 5);
  assert(b.generation(c0) == 1);
}

void exiting_peer_releases_waiter() {
  auto b = pair();
  b.arrive(c0, t0, 0, full, 1);
  b.wait(c0, t0, 0, full, 2);
  b.exit(c0, t1, 0, full, 3);
  assert(!b.waiting(c0, t0, 0));
  assert(b.generation(c0) == 1);
  b.arrive(c0, t0, 0, full, 4);
  b.wait(c0, t0, 0, full, 5);
  assert(b.generation(c0) == 2);
}

void lane_exit_updates_expected_mask() {
  auto b = pair();
  b.exit(c0, t0, 0, 0xffff0000, 1);
  b.arrive(c0, t0, 0, 0xffff, 2);
  b.wait(c0, t0, 0, 0xffff, 3);
  assert(b.waiting(c0, t0, 0));
  b.arrive(c0, t1, 0, full, 4);
  b.wait(c0, t1, 0, full, 5);
  assert(b.generation(c0) == 1);
}

void independent_clusters_and_kernels() {
  cluster_barrier_manager b;
  b.register_cta(c0, t0, 0, 32, 32);
  b.register_cta(c1, t1, 0, 32, 32);
  b.seal();
  b.arrive(c0, t0, 0, full, 1);
  b.wait(c0, t0, 0, full, 2);
  assert(b.generation(c0) == 1 && b.generation(c1) == 0);
  auto other = pair();
  other.arrive(c0, t0, 0, full, 1);
  other.wait(c0, t0, 0, full, 2);
  assert(other.waiting(c0, t0, 0));
  assert(!b.waiting(c0, t0, 0));
}

void zero_masks_have_no_effect() {
  auto b = pair();
  b.arrive(c0, t0, 0, 0, 1);
  b.wait(c0, t0, 0, 0, 2);
  b.exit(c0, t0, 0, 0, 3);
  assert(!b.waiting(c0, t0, 0));
  assert(b.generation(c0) == 0 && b.live_mask(c0, t0, 0) == full);
  b.arrive(c0, t0, 0, full, 4);
  b.wait(c0, t0, 0, full, 5);
  assert(b.waiting(c0, t0, 0));
}

void invalid_participation_is_diagnosed() {
  auto b = pair();
  rejects([&] { b.wait(c0, t0, 0, full, 1); }, "without matching arrival");
  rejects([&] { b.arrive(c0, t0, 0, 1, 1); }, "divergent");
  b.arrive(c0, t0, 0, full, 2);
  rejects([&] { b.arrive(c0, t0, 0, full, 3); }, "duplicate arrival");
  b.wait(c0, t0, 0, full, 4);
  rejects([&] { b.wait(c0, t0, 0, full, 5); }, "duplicate WAIT");
  // A released warp cannot accidentally contribute a next-generation arrival
  // while a peer has not executed its preceding WAIT.
  b.arrive(c0, t1, 0, full, 5);
  assert(!b.waiting(c0, t0, 0));
  assert(!b.arrive_ready(c0, t0, 0));
  rejects([&] { b.arrive(c0, t0, 0, full, 5); }, "duplicate arrival");
  assert(b.generation(c0) == 0);
  rejects([&] { b.waiting(c0, t0, 1); }, "unknown CTA or local warp");
  b.exit(c0, t1, 0, full, 6);
  rejects([&] { b.exit(c0, t1, 0, full, 7); }, "not live");
}

void diagnostic_identifies_missing_warp() {
  auto b = pair(64);
  b.arrive(c0, t0, 0, full, 1);
  b.wait(c0, t0, 0, full, 2);
  FILE *f = std::tmpfile();
  assert(f);
  b.dump(f);
  std::rewind(f);
  std::string text;
  char buffer[1024];
  while (std::fgets(buffer, sizeof(buffer), f)) text += buffer;
  std::fclose(f);
  assert(text.find("kernel=17 cluster=(0,0,0) generation=0") != std::string::npos);
  assert(text.find("cta=(0,0,0) rank=0 warp=0 live=ffffffff arrived=ffffffff waited=ffffffff waiting=1") != std::string::npos);
  assert(text.find("cta=(0,0,0) rank=0 warp=1 live=ffffffff arrived=00000000 waited=00000000 waiting=0") != std::string::npos);
  assert(text.find("cta=(1,0,0) rank=1 warp=1") != std::string::npos);
}

void invalid_metadata_is_diagnosed() {
  cluster_barrier_manager b;
  rejects([&] { b.arrive_ready(c0, t0, 0); }, "not been sealed");
  rejects([&] { b.register_cta(c0, t0, 0, 32, 64); }, "warp size");
  b.register_cta(c0, t0, 0, 32, 32);
  rejects([&] { b.register_cta(c1, t0, 0, 32, 32); }, "duplicate CUDA CTA");
  rejects([&] { b.register_cta(c0, t1, 0, 32, 32); }, "duplicate cluster rank");
  b.seal();
  rejects([&] { b.register_cta(c1, t1, 0, 32, 32); }, "after sealing");
  rejects([&] { b.seal(); }, "already sealed");
  cluster_barrier_manager gap;
  gap.register_cta(c0, t0, 1, 32, 32);
  rejects([&] { gap.seal(); }, "not contiguous");
}

void cycles_above_32_bits_and_audit() {
  auto b = pair();
  const uint64_t cycle = (uint64_t(1) << 40);
  setenv("CLUSTER_BARRIER_TRACE", "1", 1);
  b.arrive(c0, t0, 0, full, cycle, 0x111);
  b.wait(c0, t0, 0, full, cycle + 1, 0x222);
  b.arrive(c0, t1, 0, full, cycle + 2, 0x333);
  b.wait(c0, t1, 0, full, cycle + 3, 0x444);
  unsetenv("CLUSTER_BARRIER_TRACE");
  assert(b.generation(c0) == 1);
}
}  // namespace

int main() {
  split_phase_and_unlaunched_peer();
  multiple_generations();
  all_warps_participate();
  early_exit_and_partial_final_warp();
  exiting_peer_releases_waiter();
  lane_exit_updates_expected_mask();
  independent_clusters_and_kernels();
  zero_masks_have_no_effect();
  invalid_participation_is_diagnosed();
  diagnostic_identifies_missing_warp();
  invalid_metadata_is_diagnosed();
  cycles_above_32_bits_and_audit();
  std::cout << "12 cluster barrier state tests passed\n";
}
