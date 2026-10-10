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
gpu-simulator/build/logging-release/accel-sim.out \
  -config gpu-simulator/gpgpu-sim/configs/model-cfgs/gpgpusim_8sm_hopper.config \
  -config gpu-simulator/configs/tested-cfgs/SM90_H100/trace.config \
  -trace /path/to/kernelslist.g
```

The main config's PTX timing options do not replace the SASS timing file.
No external interconnect configuration file is needed.

## Validation and current limitation

The first eight CTAs of Gemma 4 kernel 8039, staged as an eight-CTA grid
containing two complete CUDA CTA clusters, complete successfully with this
configuration: 81,327 cycles, 3,308,972 instructions, and eight issued CTAs.
This checks Hopper TMA, GMMA, and barrier paths in one wave of resident CTAs.

The full 48-CTA kernel 8039 replay currently fails in
`ldst_unit::mbarrier_complete_tx` because a remote `STAS` reaches a peer's
mbarrier before that peer initializes it. The Hopper opcode table maps
`UCGABAR_ARV` and `UCGABAR_WAIT` to `OP_NOP`, so the trace's cluster rendezvous
does not enforce initialization ordering. Individual CTA admission exposes
this gap when later waves start peer CTAs at different times.

An isolated diagnostic replay on 2026-10-10 reproduced the failure at cycle
243,194: CTA 28, warp 3, PC `0x2a50` completes four bytes against CTA 31's
barrier `0x03000508`. CTA 31 launched at cycle 242,933 but had not initialized
that barrier. A diagnostic-only guard that defers STAS completion until all
active destinations exist released it at cycle 243,325. With that guard, all
48 CTAs completed in 413,155 cycles (19,840,968 instructions), followed by a
one-cycle successful final drain. Five STAS instructions required deferral.
Both diagnostic runs had traffic logging disabled; production binaries and
collection results were preserved.

The trace contains no mbarrier invalidations, and expected/completed byte
counts balance for all 708 transaction-bearing barriers. A proper model
repair must implement cluster arrival/wait ordering and account for cluster
admission and multicast destination readiness. The diagnostic guard is not
that repair, and its timing is not validated production output. The unchanged
full kernel remains unsupported on this model. Evidence, exact commands,
binary hashes, and diagnostic patches are retained under
`/results/mobile-traffic-20261007/completion-timing-v2/experiments/8039-mobile-debug-20261010/`
(`diagnosis.json`).

Commands, config snapshots, and results are retained under the framework's
`data/gemma4-h100/runs-mobile-hopper-8sm-single-wave-smoke/` and
`data/gemma4-h100/runs-mobile-hopper-8sm-config-smoke/` directories.
