# CV610 Optical Flow

Status: **Phase A — measurement**. The benchmark tool is in the tree; the
numbers it produces decide whether Phase B (the pipeline port) is built.

## Why a port, not a merge

The `OptFlow` branch (a6d8494, on v0.10.11) gives the Star6E backend a
relative image-motion estimate — `tx`/`ty` in encoded pixels plus an
image-plane rotation proxy `rz` — using SigmaStar's `MI_IVE_LkOpticalFlow`,
drawn on the debug OSD. Reading it closely, far less of it runs on hardware
than its design note says:

- **LK mode:** only the LK iteration itself is on IVE, single pyramid level,
  at most 14 points (the vendor call crashes at 16). Two ~1 MB frame copies,
  the nearest-neighbour downscale to 160x180, Harris corner scoring over
  ~3.3k windows and the reduction are all CPU, part of it in the encoder
  thread.
- **SAD mode:** `MI_IVE_Sad` produces a motion ROI that is only logged; every
  output number comes from a CPU patch matcher, run synchronously in the
  encoder thread, plus a ~2 MB copy into uncached memory per solve.
- **No measurements exist.** The branch defines a perf log but no captured
  output is recorded anywhere, and the roadmap figures (LK 2–5 ms at 10 Hz)
  are budgets, not readings.

Master is 0.84.0 and the CV610 backend did not exist when the branch was cut,
so this is a re-implementation on `src/cv610_*` that lifts only the portable
pieces: the tx/ty/rz reduction, the `optflow.*` knobs and the `debug_osd_*`
drawing calls.

## What the Hi3516CV610 offers

Verified from the CV6xx headers in OpenIPC's `openhisilicon` checkout and
from the vendor libraries shipped by `hisilicon-osdrv-hi3516cv6xx`:

| Capability | CV610 | Fit |
|---|---|---|
| `ss_mpi_ive_lk_optical_flow_pyr` | ≤500 points, `max_level` 0–3, `iter_cnt` ≤20, level 0 64x64..1280x720 U8C1, stride ×16, caller builds the pyramid | Replaces the single-level ≤14-point LK |
| `ss_mpi_ive_st_cand_corner` (HW) + `ss_mpi_ive_st_corner` (CPU pick) | ≤500 corners | Replaces CPU Harris |
| `ss_mpi_ive_resize` (bilinear) / `ss_mpi_ive_dma` interval copy | — | Pyramid in hardware, no CPU downscale |
| `ss_mpi_ive_sad` / `ss_mpi_ive_ncc` | ≤1080p | Same-position energy only — no displacement search |
| DIS / GME, encoder motion vectors, KCF | **not supported** on CV610 | No free global-motion source |
| NPU (`svp_acl_*`, ~1 TOPS) | `.om` models via ATC | Possible later; heavy toolchain, deferred |

A dedicated low-resolution luma-only VPSS channel can deliver the tap frame
with zero CPU work, and the OSD on CV610 composites at VENC, so a VPSS tap
is overlay-free. Everything the Star6E branch does on the CPU per solve
therefore moves to VPSS/IVE; what remains is the corner pick and a
reduction over ≤100 points. That is why a CV610 port is expected to cost
less CPU and track better — expected, which is what Phase A measures.

## Phase A: `tools/cv610_ive_bench.c`

```sh
make cv610-ive-bench SOC_BUILD=cv610 CV610_CC=... CV610_SDK_INC=... CV610_SDK_LIB=...
scp -O out/cv610/cv610_ive_bench root@<bench>:/tmp/
ssh root@<bench> '/etc/init.d/S95waybeam stop; /tmp/cv610_ive_bench'
```

The MPP modules stay loaded; `init.d/load-cv610-online` now loads `open_ive
save_power=1` (non-fatal). The tool brings up VB/SYS itself the way the
daemon does, allocates uncached MMZ, and for each level-0 size (default
320x180, 640x360, 1280x720) builds a low-pass random texture and a copy
shifted by (5, −3) px with ±2 counts of noise. It then prints one line per
measurement, `wall_us=min/med/max cpu_us=min/med/max` over `--iters` runs:

| `op=` | What is timed |
|---|---|
| `dma_copy_l0` | Frame → level-0 copy, as the port lifts the luma out of the VPSS frame |
| `resize_levels` / `dma_decimate_levels` | The two ways of building levels 1..n |
| `st_cand_corner_hw` / `st_corner_cpu` | Corner eigenvalues (IVE) and the corner pick (CPU) |
| `lk_pyr points=N max_level=L` | LK alone, per point count and level; levels above the size limit are tried once so the refusal code is on record |
| `chain_decimate` / `chain_resize` | The port's whole per-solve submission chained with one blocking query, then the corner pick |

Each LK line also reports `status1=`/`status0=`/`other=` (which status value
means "tracked" on this SoC) and `med_err_px=`, the median distance between
the measured flow and the known shift over tracked points. A vendor call
that fails prints `ret=0x...` and the run continues — the return codes for
the auxiliary-buffer sizes (`--resize-mem`) are part of what the tool is for.

Pyramid levels per size follow from the 64x64 minimum: 320x180 allows
`max_level` 1, 640x360 allows 2, 1280x720 allows 3.

### Decision rule

Go to Phase B when, at 640x360 with 100 points, the `chain_*` line shows
wall ≤ 5 ms and CPU ≤ 1.5 ms (median), `med_err_px` ≤ 0.5, at least 80 % of
points tracked, and `dmesg` shows no IVE errors. If wall passes but CPU does
not, re-run at 320x180 (`max_level` 1) and accept ≤ 1 ms there. No-go when
`open_ive` does not load, an operator fails regardless of buffer sizing, or
accuracy fails at both sizes.

For the Star6E side of the comparison, run the `OptFlow` branch on the Star6E
bench once with `optflow.verbose=true` and read `total_ms/runs` from its
`[optflow] perf:` line.

### Results

_Not yet run on hardware. Paste the bench output here, with the firmware and
`libss_mpi_ive.so` checksum, when it has been._

## Phase B outline (not built)

- **Tap:** VPSS physical channel 1, USER mode, `OT_PIXEL_FORMAT_YUV_400`
  (NV21 fallback), `depth=2`, default 640x360 (`max_level` 2), its own VB
  pool; consumed with `ss_mpi_vpss_get_chn_frame` on a worker thread,
  rate-limited to `optflow.fps`.
- **Per solve:** DMA copy of the luma plane to level 0, pyramid by resize or
  decimation (whichever Phase A favours), `lk_optical_flow_pyr`
  prev→cur, `st_cand_corner` on the new frame for the next solve, one
  blocking query, release the VPSS frame, CPU `st_corner`, reduce.
- **Reduction:** SDK-free `optflow_reduce.c` with host tests — median/MAD
  inlier selection over ~100 points, then the branch's tx/ty/rz formulas.
- **Output:** debug OSD only — red accumulated-motion marker, green inlier
  and red outlier points — drawn in the same begin/end bracket as the text
  rows, refreshed whenever a new solve lands.
- **Config:** `optflow.{enabled,mode,showOsd,verbose,fps,points}`, CV610-only
  in `/api/v1/capabilities`, validated in `cv610_validation.c`.

### Assumptions Phase A settles

Whether a query on the last chained handle implies the earlier ones are
done; the `st_cand_corner` and `resize` auxiliary-buffer size rules; LK
status polarity; the IVE `/dev` node name for `MPP_DEVICES`. Still open after
Phase A: whether a VPSS physical channel accepts `YUV_400` in USER mode, and
whether a full channel-1 user list drops frames without stalling channel 0.
