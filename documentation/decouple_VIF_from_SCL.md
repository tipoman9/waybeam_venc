# Decoupling VIF Rate from SCL/VENC Rate

## Concept

The sensor runs at its driver-native fps (determined by HMAX/VMAX in the sensor init
table).  The configured fps is applied downstream via the SCL→VENC RING bind's
`DstFrmrate`.  SCL drops the excess frames before they reach the encoder.

```
Sensor / VIF  ──► ISP ──► SCL (frame drop) ──► VENC ──► output
  native fps               src=native fps        encode fps
                           dst=encode fps
```

This is how majestic operates.  Observed example with IMX335 mode 2 (driver-native
fps ≈ 41), configured fps = 38:

| Stage          | fps |
|----------------|-----|
| Sensor / VIF   | **41 fps** (native from HMAX/VMAX) |
| SCL DstFrmrate | **38 fps** (configured value) |
| VENC output    | **38 fps** |

The RING buffer absorbs the rate mismatch.  The encoded video carries the configured
fps — this is not a display-only value — but the sensor timing is never disturbed.

## Old waybeam behaviour (before this change)

Waybeam called `MI_SNR_SetFps(configured_fps)`, which overrides VMAX so the sensor
runs at the configured rate.  VIF and VENC both showed the same fps.

| Stage        | fps (cfg=38, native=41) |
|--------------|------------------------|
| VIF          | **38 fps** (SetFps overrode VMAX) |
| SCL src=dst  | **38 fps** |
| VENC output  | **38 fps** |

## Changes made to implement majestic parity

### `sensor_select.c` — `set_sensor_fps`

`set_sensor_fps` now targets `mode->maxFps` (the driver-native rate) instead of
`target_fps` (the configured value).  This leaves VMAX at the init-table setting so
the sensor runs at its native blanking interval.

**Exception — ISP FIFO FULL protection:**  At very high MIPI bandwidth (mode 3,
1188 Mbps, native 60 fps), the short inter-frame blanking at native fps can overflow
the ISP input FIFO.  When the native fps exceeds the configured fps by 10 or more,
the code falls back to calling `SetFps(configured_fps)` instead.  The higher VMAX
this produces gives the ISP enough blanking time to drain.

```c
uint32_t hw_fps = (target_fps > 0 && native_fps >= target_fps + 10)
                  ? sensor_mode_clamp_fps(mode, target_fps)
                  : native_fps;
```

Threshold examples:

| Mode | native | cfg | native ≥ cfg+10? | hw_fps (VIF) |
|------|--------|-----|-------------------|--------------|
| 2    | 45     | 38  | 45 ≥ 48? No       | 45 (native)  |
| 2    | 45     | 42  | 45 ≥ 52? No       | 45 (native)  |
| 3    | 60     | 42  | 60 ≥ 52? Yes      | 42 (cfg)     |
| 3    | 60     | 50  | 60 ≥ 60? Yes      | 50 (cfg)     |

The `SensorSelectResult.fps` field now always holds the hw fps that was actually
applied to the sensor (native or clamped), not the configured fps.

### `maruko_pipeline.c` — encode_fps and SCL bind

`encode_fps` is computed as `min(configured_fps, sensor.fps)`:

```c
uint32_t cfg_fps  = ctx->cfg.sensor_fps ? ctx->cfg.sensor_fps : ctx->sensor.fps;
ctx->encode_fps   = (cfg_fps > 0 && cfg_fps < ctx->sensor.fps)
                    ? cfg_fps : ctx->sensor.fps;
```

The SCL→VENC RING bind uses `src = sensor.fps` and `dst = encode_fps`:

```c
MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
    ctx->sensor.fps, ctx->encode_fps, I6_SYS_LINK_RING, 0);
```

When `encode_fps == sensor.fps` (no drop needed), src=dst and the bind behaves
identically to before.

**VPE started before VIF** — once VIF's output port is enabled the sensor starts
pushing MIPI data into the ISP input FIFO.  Starting ISP+SCL (VPE) first ensures
there is a consumer ready before data arrives, preventing the startup FIFO FULL burst
that triggered "ISP P0 FIFO FULL" errors at high MIPI rates.

### `maruko_pipeline.h` — encode_fps comment

The `encode_fps` field in `MarukoBackendContext` is documented to reflect its new
role: video output fps (≤ sensor.fps), while `sensor.fps` is always the driver-native
hw rate.

### IMX335 driver — mode 2 metadata (drv_ms_cus_imx335_MIPI.c)

Mode 2 was updated to 45 fps (HMAX=0x016A=362, 891 Mbps).  Three driver variables
must be kept consistent with the HMAX value:

| Variable             | Formula                          | Value  |
|----------------------|----------------------------------|--------|
| `Preview_MAX_FPS`    | floor(pixel_clk / (HMAX × VMAX_min)) | 45 |
| `vts_30fps`          | pixel_clk / (max_fps × HMAX)    | 4619   |
| `Preview_line_period`| HMAX × 1e9 / pixel_clk (ns)     | 4812   |

`pixel_clk` for mode 2 at 891 Mbps = 75 240 000 Hz (derived from
`max_fps × HMAX × VMAX_at_max_fps = 45 × 362 × 4619`).

Whenever HMAX changes in `Sensor_init_table_4lane_5m60fps`, recalculate all three
values.  Wrong values silently corrupt VMAX inside `pCus_SetFPS`, producing incorrect
sensor timing.  See memory `imx335_mode2_fps_fix` for the fix recipe.

## Reference: majestic proc dump (IMX335 mode 2, ~59fps driver setting)

Captured via `cat /proc/mi_modules/mi_vif/mi_vif0`, `mi_scl0`, `mi_venc0`.

### VIF
| Field         | Value |
|---------------|-------|
| Cap/output    | **2560×1920** (VIF crops ~32 px/side from 2592×1944 active area) |
| Pixel format  | GB\_10BPP (raw Bayer 10-bit into ISP) |
| Bind to ISP   | bind\_type = REALTIME (4) |
| Actual fps    | **~58 fps** (sensor native from HMAX/VMAX) |

### ISP → SCL
| Field          | Value |
|----------------|-------|
| Input          | 2560×1920, REALTIME |
| Output         | **1920×1080**, YUV420SP |
| Ring compress  | **6** (IFC compressed) |
| Bind to VENC   | bind\_type = **Ring (16)** |
| SCL SrcFrmrate | **60/1** |
| SCL DstFrmrate | **60/1** |

### VENC
| Field           | Value |
|-----------------|-------|
| Input           | 1920×1080, SrcFrmrate=60/1, DstFrmrate=60/1 |
| Encoded fps     | **~59 fps** (tracks real sensor delivery) |
| Codec           | H.265 CBR 3.6 Mbps, GOP 30 |
