# Mobile GPU models

`gpgpusim_8sm.config` is the existing Ampere SM87-like mobile model.
`gpgpusim_8sm_hopper.config` combines its mobile system configuration with
the repository's H100 SM90 core settings. These are approximate models.

Both use eight SMs, four 32-bit LPDDR-style channels, eight L2 subpartitions,
a 2 MiB L2, one chiplet, and the built-in local crossbar. The Hopper version
retains the mobile clocks and detailed DRAM model. It adds Hopper core
resources, 256 KiB unified L1/shared memory per SM (up to 228 KiB shared),
GMMA support, and LRC with up to four contributors per forwarded request.

For Hopper SASS replay, load the existing H100 trace timing file as well.
From the framework root:

```bash
gpu-simulator/build/cluster-release/accel-sim.out \
  -config gpu-simulator/gpgpu-sim/configs/model-cfgs/gpgpusim_8sm_hopper.config \
  -config gpu-simulator/configs/tested-cfgs/SM90_H100/trace.config \
  -trace /path/to/kernelslist.g \
  -gpgpu_max_concurrent_kernel 1 -gpgpu_concurrent_kernel_sm 0
```

The main config's PTX timing options do not replace the SASS timing file.
No external interconnect configuration file is needed.

## Cluster synchronization

The `fix/hopper-cluster-barriers` branch implements Hopper `UCGABAR_ARV`
and `UCGABAR_WAIT` as split-phase cluster synchronization. State is tracked
per kernel, CUDA cluster and generation. Arrival orders preceding synchronous
memory operations, including mbarrier initialization; WAIT holds the warp until
all live participants have arrived. The state follows NVIDIA's
[cluster barrier semantics](https://docs.nvidia.com/cuda/archive/12.1.1/parallel-thread-execution/index.html#parallel-synchronization-and-communication-instructions-barrier-cluster).
TMA, LDGSTS and GMMA completion still use their
separate asynchronous synchronization mechanisms. This uses the existing cache
model and does not add global cache coherence: `CCTL.IVALL` still does not
invalidate L1, so timing for a peer store followed by a cached load retains
that limitation.

Cluster replay currently requires `-gpgpu_max_concurrent_kernel 1` and
`-gpgpu_concurrent_kernel_sm 0`. Before launching an active cluster kernel,
text, xz and tracez preflight verifies complete CTA/cluster/local-rank metadata,
dense cluster ranks, contiguous cluster members in trace order, nonempty warp
traces, and arrival/wait pairing for surviving lanes. Unsupported opcode modifiers
and divergent masks are rejected; short final warps and previously exited lanes
are accounted for when checking the remaining live participants.
The cluster must fit the configuration's total resident CTA capacity, otherwise
launch fails with a diagnostic. These checks let the existing FIFO CTA scheduler
make progress across waves. General cluster reordering, concurrent clustered
kernels and GPC placement are outside this implementation.

Previously, both instructions were decoded as no-ops. A later-wave CTA in full
Gemma kernel 8039 could therefore issue remote STAS before a peer initialized its
mbarrier on the eight-SM model. The isolated
[historical diagnosis](/results/mobile-traffic-20261007/completion-timing-v2/experiments/8039-mobile-debug-20261010/diagnosis.json)
records the original assertion and a temporary STAS guard used to establish the
cause. That guard is not part of the implementation.

## Isolated verification

Both the framework and nested GPGPU-Sim repository use branch
`fix/hopper-cluster-barriers` in `/tmp/hopper-cluster-barriers`; the original
workspace and collection binaries remain separate. The verification runner
pins its binary, library, configs and validators in each fresh output directory.
From that worktree, run generated synchronization fixtures with:

```bash
python3 util/logging-regression/tests/run-cluster-barriers.py \
  --binary gpu-simulator/build/cluster-release/accel-sim.out \
  --library-dir gpu-simulator/build/cluster-release/gpgpu-sim \
  --mode synthetic --output /path/to/new-cluster-fixture-results
```

Use `--mode captured --cases mobile-hopper:8039` for the full mobile replay, with
`--hopper-traces /path/to/hopper/traces` when traces are stored elsewhere.
Captured checks compare logging-on/off execution and require full completion
timing coverage after memory draining. Parser-only tests are available through
`python3 util/logging-regression/tests/test_cluster_trace_parser.py -v`.

Current verification artifacts, commands and source hashes are collected under
`/results/mobile-traffic-20261007/completion-timing-v2/experiments/cluster-barriers-20261010-012835/`.
Per-case reports distinguish successful replays from any remaining simulation
failures; the historical collections are preserved.

### Verified results (2026-10-10)

Full Gemma kernel 8039 passes on the unchanged mobile Hopper model, H100 and
H200 without the temporary STAS-deferral guard. Every configuration completes
all 48 CTAs and 19,840,968 instructions, with 576 arrivals, 576 waits and 576
releases across 12 cluster generations. All 36 remote STAS completions follow
their destination mbarrier initialization. Strict validation finds exactly
504,000 ingress transports and 504,000 timing records per configuration.
Logging-on/off architectural statistics and final-drain reports match.

| Configuration | Kernel finish cycle | Additional drain cycles | Drain end cycle |
| --- | ---: | ---: | ---: |
| Mobile Hopper, 8 SMs | 411,320 | 0 | 411,320 |
| H100 | 13,383 | 95 | 13,478 |
| H200 | 12,239 | 78 | 12,317 |

The original mobile failure is now ordered: CTA 31 initializes barrier
`0x03000508` at cycle 237,637; CTA 28 warp 3 leaves its cluster wait at 237,652;
the remote STAS completes at 238,237. The event witness is preserved with the
mobile 8039 result.

The Ampere ResNet kernel 26 control retains its baseline architectural
statistics and covers all 131,584 bypass loads and 100,352 L1 stores. Focused
verification passes 56 text/tracez runtime replays, 21 parser tests covering
65 format cases, 12 sanitized state tests, nine audit-validator tests, and the
existing completion-logging regression suites.

Mobile Hopper kernels 6542, 9870 and 13076 still deadlock after their initial
96 arrivals, waits and releases complete. Their unfinished-thread vectors
match the original runs, with no participant left waiting on the cluster
barrier. The exact later mbarrier/TMA stall remains unresolved; these failed
runs have no timing-completeness claim. See the experiment's
`remaining-tma-deadlocks.json` for evidence and `summary.json` for the complete
acceptance report. Existing collections and production binaries are unchanged.
