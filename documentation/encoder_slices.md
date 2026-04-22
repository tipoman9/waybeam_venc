# H265 Slice Split — Implementation Notes

## Overview

Slice split divides each encoded frame into multiple independent NAL units
(slices) that can be sent and decoded as soon as they are ready, without
waiting for the full frame to complete. This reduces end-to-end latency
at the cost of slightly lower compression efficiency.

On SigmaStar star6e the feature is exposed via `MI_VENC_SetH265SliceSplit`.

---

## Configuration

Add `sliceRows` to `video0` in the JSON config (or via `--set`):

```json
"video0": {
  "sliceRows": 4
}
```

`sliceRows` is the number of macroblock rows per slice. Each macroblock row is
16 pixels tall. For 1080p (68 MB rows):

| sliceRows | slices/frame | slice height |
|-----------|-------------|--------------|
| 0         | 1 (off)     | —            |
| 4         | ~17         | 64 px        |
| 8         | ~9          | 128 px       |
| 17        | 4           | 272 px       |
| 34        | 2           | 544 px       |

Default is `0` (slicing disabled, legacy behavior).

---

## Implementation

### Config layer (`include/venc_config.h`, `src/venc_config.c`)

`VencConfigVideo.slice_rows` (int). Parsed from `sliceRows`, clamped to ≥ 0.
Serialized back on GET.

### API layer (`src/venc_api.c`)

Field `video0.slice_rows` with camelCase alias `video0.sliceRows`.
Mutation type `MUT_RESTART` — requires encoder restart to take effect.

### MI layer (`include/star6e_mi.h`, `src/star6e_mi.c`)

Two optional function pointers in `star6e_venc_impl`:

```c
int (*fnSetH265SliceSplit)(int chn, void *param);
int (*fnSetH264SliceSplit)(int chn, void *param);
```

Loaded with bare `dlsym` after the required symbols pass validation. If the
firmware does not export these symbols the pointers are NULL and slicing is
silently skipped — the encoder runs in full-frame mode.

### Pipeline layer (`src/star6e_pipeline.c`)

After `MI_VENC_CreateChn`, if `slice_rows > 0` and the matching function
pointer is non-NULL:

```c
struct { int bSplitEnable; uint32_t u32SliceRowCount; } sp = {
    .bSplitEnable = 1,
    .u32SliceRowCount = (uint32_t)slice_rows,
};
g_mi_venc.fnSetH265SliceSplit(chn, &sp);
```

The struct is defined inline to avoid a dependency on the vendor SDK header.
The dual-stream (ch1 recording) channel is always started with `slice_rows=0`.

### RTP layer (`src/star6e_hevc_rtp.c`, `include/star6e_hevc_rtp.h`)

`star6e_hevc_rtp_send_frame` gains an `end_of_frame` parameter (1 = last or
only slice of the frame, 0 = intermediate slice).

- The RTP **marker bit** is set only on the last RTP packet of the last slice
  (`end_of_frame=1`). This is required by RFC 7798.
- The RTP **timestamp** is advanced only when `end_of_frame=1`, so all slices
  of one frame share the same timestamp.
- The AP (Aggregation Packet) builder is flushed with `marker=end_of_frame`.

### Video state layer (`include/star6e_video.h`, `src/star6e_video.c`)

`Star6eVideoState.slices_enabled` is set from `vcfg->video0.slice_rows > 0`
during `star6e_video_init()`.

When `slices_enabled`, `send_frame_output_rtp` reads `endFrame` from the last
`MI_VENC_Pack_t` of the stream to compute `end_of_frame`. When slices are
disabled `end_of_frame` is always 1 (backward-compatible).

---

## The `endFrame` field

`MI_VENC_Pack_t.endFrame` (char) is set to 1 by the driver on the last pack of
the last slice of a frame, and 0 on intermediate slices. In non-slice mode its
value is unspecified; the code ignores it (`end_of_frame=1` always) in that
case.

---

## Validation

Tested on star6e (IMX335, 1920×1080 @ 60fps, H265 CBR 8 Mbps) with `sliceRows=4`:

- Encoder runs stably, no crashes, no WFB TX errors.
- GStreamer decode pipeline (`rtph265depay ! h265parse ! avdec_h265`) produced
  **1920×1080 @ ~59fps, dropped=0** over 8 seconds of continuous streaming.
- `h265parse` assembled slices into complete AUs without errors.

GStreamer test pipeline:

```
gst-launch-1.0 -v \
  udpsrc port=5600 buffer-size=65536 \
    caps="application/x-rtp,encoding-name=H265,payload=96" \
  ! rtph265depay ! h265parse ! avdec_h265 \
  ! videoconvert ! fpsdisplaysink video-sink=fakesink sync=false
```

---

## Limitations

- WebUI does not expose `sliceRows` (the WebUI is a pre-compiled gzip blob
  with no source in this repository).
- H264 slice split (`fnSetH264SliceSplit`) is wired but untested.
- The dual-stream ch1 (recording channel) always runs without slices.
- The firmware `MI_VENC_SetH265SliceSplit` symbol may be absent on older
  builds; in that case slicing is silently disabled and full-frame mode is used.
