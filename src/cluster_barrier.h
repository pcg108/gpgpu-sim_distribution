// Functional split-phase cluster barriers for trace-driven Hopper execution.
// Timing of preceding synchronous memory operations is enforced by the caller.
#ifndef GPGPUSIM_CLUSTER_BARRIER_H
#define GPGPUSIM_CLUSTER_BARRIER_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

struct cluster_barrier_coord {
  unsigned x, y, z;
  cluster_barrier_coord(unsigned a = 0, unsigned b = 0, unsigned c = 0)
      : x(a), y(b), z(c) {}
  template <class Coord>
  cluster_barrier_coord(const Coord &c) : x(c.x), y(c.y), z(c.z) {}
  bool operator<(const cluster_barrier_coord &c) const {
    return std::tie(x, y, z) < std::tie(c.x, c.y, c.z);
  }
};

class cluster_barrier_manager {
 public:
  void set_kernel_uid(unsigned uid) { kernel_uid_ = uid; }
  bool enabled() const { return sealed_ && !clusters_.empty(); }

  // Register every CTA before execution, including currently unlaunched CTAs.
  void register_cta(cluster_barrier_coord cluster, cluster_barrier_coord cta,
                    unsigned rank, unsigned threads, unsigned warp_size) {
    if (sealed_) fail("cannot register CTA after sealing membership", cluster);
    if (!threads || !warp_size || warp_size > 32)
      fail("invalid CTA threads or warp size", cluster);
    if (cta_clusters_.count(cta)) fail("duplicate CUDA CTA metadata", cluster);
    auto &s = clusters_[cluster];
    for (const auto &entry : s.ctas)
      if (entry.second.rank == rank) fail("duplicate cluster rank", cluster);
    cta_state member;
    member.rank = rank;
    for (unsigned first = 0; first < threads; first += warp_size) {
      const unsigned lanes = std::min(warp_size, threads - first);
      warp_state w;
      w.live = lanes == 32 ? UINT32_MAX : ((uint32_t(1) << lanes) - 1);
      member.warps.push_back(w);
    }
    s.ctas.emplace(cta, std::move(member));
    cta_clusters_.emplace(cta, cluster);
  }

  void seal() {
    if (sealed_) throw std::runtime_error("cluster barrier membership already sealed");
    for (const auto &entry : clusters_) {
      std::vector<bool> ranks(entry.second.ctas.size(), false);
      for (const auto &cta : entry.second.ctas) {
        if (cta.second.rank >= ranks.size())
          fail("cluster ranks are not contiguous from zero", entry.first);
        ranks[cta.second.rank] = true;
      }
    }
    sealed_ = true;
  }

  unsigned max_ctas_per_cluster() const {
    unsigned result = 0;
    for (const auto &entry : clusters_)
      result = std::max(result, unsigned(entry.second.ctas.size()));
    return result;
  }

  // A warp may pass WAIT as soon as all arrived, but must not arrive at the
  // next phase until every surviving participant executed its previous WAIT.
  bool arrive_ready(cluster_barrier_coord cluster, cluster_barrier_coord cta,
                    unsigned warp) const {
    return member(cluster, cta, warp).waited == 0;
  }

  void arrive(cluster_barrier_coord cluster, cluster_barrier_coord cta,
              unsigned warp, uint32_t mask, uint64_t cycle, uint64_t pc = 0) {
    if (!mask) return;
    auto &s = state(cluster);
    auto &w = member(cluster, cta, warp);
    check_mask(w, mask, cluster, cta, warp);
    if (w.arrived) fail("duplicate arrival before phase completion", cluster, cta, warp);
    w.arrived = mask;
    audit("arrive", cluster, s.generation, cta, warp, mask, cycle, pc);
    progress(cluster, s, cycle);
  }

  void wait(cluster_barrier_coord cluster, cluster_barrier_coord cta,
            unsigned warp, uint32_t mask, uint64_t cycle, uint64_t pc = 0) {
    if (!mask) return;
    auto &s = state(cluster);
    auto &w = member(cluster, cta, warp);
    check_mask(w, mask, cluster, cta, warp);
    if ((w.arrived & mask) != mask)
      fail("WAIT without matching arrival", cluster, cta, warp);
    if (w.waited) fail("duplicate WAIT in one generation", cluster, cta, warp);
    w.waited = mask;
    w.pending_wait = true;
    w.wait_pc = pc;
    audit("wait", cluster, s.generation, cta, warp, mask, cycle, pc);
    progress(cluster, s, cycle);
  }

  bool waiting(cluster_barrier_coord cluster, cluster_barrier_coord cta,
               unsigned warp) const {
    return member(cluster, cta, warp).pending_wait;
  }

  void exit(cluster_barrier_coord cluster, cluster_barrier_coord cta,
            unsigned warp, uint32_t mask, uint64_t cycle) {
    if (!mask) return;
    auto &s = state(cluster);
    auto &w = member(cluster, cta, warp);
    if ((w.live & mask) != mask)
      fail("exiting lanes are not live", cluster, cta, warp);
    w.live &= ~mask;
    if (!w.live) w.pending_wait = false;
    audit("exit", cluster, s.generation, cta, warp, mask, cycle, 0);
    progress(cluster, s, cycle);
  }

  uint64_t generation(cluster_barrier_coord cluster) const {
    return state(cluster).generation;
  }
  uint32_t live_mask(cluster_barrier_coord cluster, cluster_barrier_coord cta,
                     unsigned warp) const {
    return member(cluster, cta, warp).live;
  }

  void dump(FILE *fp) const {
    for (const auto &entry : clusters_) {
      const auto &c = entry.first;
      const auto &s = entry.second;
      fprintf(fp, "cluster barrier kernel=%u cluster=(%u,%u,%u) generation=%llu\n",
              kernel_uid_, c.x, c.y, c.z,
              static_cast<unsigned long long>(s.generation));
      for (const auto &cta : s.ctas)
        for (unsigned i = 0; i < cta.second.warps.size(); ++i) {
          const auto &w = cta.second.warps[i];
          if (!w.live) continue;
          fprintf(fp, "  cta=(%u,%u,%u) rank=%u warp=%u live=%08x arrived=%08x waited=%08x waiting=%u\n",
                  cta.first.x, cta.first.y, cta.first.z, cta.second.rank,
                  i, w.live, w.arrived, w.waited, w.pending_wait);
        }
    }
  }

 private:
  struct warp_state {
    uint32_t live = 0, arrived = 0, waited = 0;
    bool pending_wait = false;
    uint64_t wait_pc = 0;
  };
  struct cta_state {
    unsigned rank = 0;
    std::vector<warp_state> warps;
  };
  struct cluster_state {
    uint64_t generation = 0;
    std::map<cluster_barrier_coord, cta_state> ctas;
  };
  unsigned kernel_uid_ = 0;
  bool sealed_ = false;
  std::map<cluster_barrier_coord, cluster_state> clusters_;
  std::map<cluster_barrier_coord, cluster_barrier_coord> cta_clusters_;

  [[noreturn]] void fail(const char *reason, cluster_barrier_coord cluster,
                        cluster_barrier_coord cta = {}, unsigned warp = 0) const {
    std::ostringstream out;
    out << "cluster barrier: " << reason << " (kernel=" << kernel_uid_
        << " cluster=" << cluster.x << ',' << cluster.y << ',' << cluster.z
        << " cta=" << cta.x << ',' << cta.y << ',' << cta.z
        << " warp=" << warp << ')';
    throw std::runtime_error(out.str());
  }
  const cluster_state &state(cluster_barrier_coord cluster) const {
    if (!sealed_) fail("membership has not been sealed", cluster);
    auto it = clusters_.find(cluster);
    if (it == clusters_.end()) fail("unknown cluster", cluster);
    return it->second;
  }
  cluster_state &state(cluster_barrier_coord cluster) {
    return const_cast<cluster_state &>(
        static_cast<const cluster_barrier_manager *>(this)->state(cluster));
  }
  const warp_state &member(cluster_barrier_coord cluster,
                           cluster_barrier_coord cta, unsigned warp) const {
    const auto &s = state(cluster);
    auto it = s.ctas.find(cta);
    if (it == s.ctas.end() || warp >= it->second.warps.size())
      fail("unknown CTA or local warp", cluster, cta, warp);
    return it->second.warps[warp];
  }
  warp_state &member(cluster_barrier_coord cluster, cluster_barrier_coord cta,
                     unsigned warp) {
    return const_cast<warp_state &>(
        static_cast<const cluster_barrier_manager *>(this)->member(cluster, cta, warp));
  }
  void check_mask(const warp_state &w, uint32_t mask,
                  cluster_barrier_coord cluster, cluster_barrier_coord cta,
                  unsigned warp) const {
    if (mask != w.live)
      fail("divergent UCGABAR mask must equal all live warp lanes", cluster, cta, warp);
  }

  void progress(cluster_barrier_coord cluster, cluster_state &s,
                uint64_t cycle) {
    bool all_arrived = true, all_waited = true, any_activity = false;
    for (const auto &cta : s.ctas)
      for (const auto &w : cta.second.warps) {
        all_arrived &= (w.arrived & w.live) == w.live;
        all_waited &= (w.waited & w.live) == w.live;
        any_activity |= w.arrived != 0 || w.waited != 0;
      }
    if (!all_arrived) return;
    for (auto &cta : s.ctas)
      for (unsigned i = 0; i < cta.second.warps.size(); ++i) {
        auto &w = cta.second.warps[i];
        if (w.pending_wait) {
          audit("release", cluster, s.generation, cta.first, i,
                w.waited & w.live, cycle, w.wait_pc);
          w.pending_wait = false;
        }
      }
    if (all_waited && any_activity) {
      if (s.generation == UINT64_MAX) fail("generation overflow", cluster);
      ++s.generation;
      for (auto &cta : s.ctas)
        for (auto &w : cta.second.warps) {
          w.arrived = 0;
          w.waited = 0;
          w.wait_pc = 0;
        }
    }
  }

  void audit(const char *event, cluster_barrier_coord cluster,
             uint64_t generation, cluster_barrier_coord cta, unsigned warp,
             uint32_t mask, uint64_t cycle, uint64_t pc) const {
    const char *enabled = std::getenv("CLUSTER_BARRIER_TRACE");
    if (!enabled || std::string(enabled) != "1") return;
    fprintf(stderr,
            "CLUSTER_BARRIER {\"event\":\"%s\",\"kernel_uid\":%u,\"cluster\":[%u,%u,%u],\"generation\":%llu,\"cta\":[%u,%u,%u],\"warp\":%u,\"cycle\":%llu,\"mask\":%u,\"pc\":%llu}\n",
            event, kernel_uid_, cluster.x, cluster.y, cluster.z,
            static_cast<unsigned long long>(generation), cta.x, cta.y, cta.z,
            warp, static_cast<unsigned long long>(cycle), mask,
            static_cast<unsigned long long>(pc));
  }
};

#endif
