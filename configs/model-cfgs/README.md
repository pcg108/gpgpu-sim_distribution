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
all live participants have arrived. TMA and LDGSTS completion still use their
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
