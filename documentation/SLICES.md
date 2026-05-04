# H265 Slice Split — Implementation Notes

## Overview

Slice split divides each encoded frame into multiple independent NAL units
(slices) that can be sent and decoded as soon as they are ready, without
waiting for the full frame to complete. This reduces end-to-end latency
at the cost of slightly lower compression efficiency.

On SigmaStar star6e the feature is exposed via `MI_VENC_SetH265SliceSplit`.

---

## Hardware Status

**SSC338Q / SSC30KQ (tested firmware, 2026-05)**

`MI_VENC_SetH265SliceSplit` is present and callable; the function returns 0.
However, the encoder produces only **one VCL NAL per frame** regardless of the
`sliceRows` setting.  Confirmed by three independent measurements:

- **Bitstream-level slice header parsing** (`rtp_slice_check.py`, 2026-05-04):
  every VCL NAL has `first_slice_segment_in_pic_flag=1` and
  `slice_segment_address=0`.  The SPS was decoded correctly
  (1920×1080, CTB=64 px, 30×17=510 CTBs, 9-bit address field), so the
  address field is being read with the right bit width.  A real multi-slice
  encoder would set the flag to 0 on slices 1…N and increment the address by
  30 CTBs per row (address 0, 30, 60, … for 1080p with CTB=64).  No such
  pattern was ever observed across 53 captured frames with `sliceRows=4`.

- **RTP timestamp spacing**: all timestamp groups are spaced exactly
  `frame_ticks=1500` apart — no intra-frame steps that would indicate
  per-slice AU delivery.

- **Internal stream diagnostic**: `stream->count=1, packNum=4` (VPS, SPS, PPS,
  and a single VCL covering the entire frame, length ≈26 KB).

The H.265 hardware encoder on these chips does not implement multi-slice output
at the SDK boundary.  The `MI_VENC_SetH265SliceSplit` call is accepted silently
but has no effect on the bitstream.

**Practical consequence for `perSliceAu=true`**:  the RTP framing works
correctly — each VCL is wrapped as its own AU — but with one VCL per frame
the result is one AU per frame.  A lost packet still freezes the entire frame;
row-band isolation is not achieved.  The `perSliceAu` encoder path and the
receiver-side reassembler (`tools/hevc_slice_reassembler.py`) are both correct
and will deliver row-band isolation automatically if a future firmware or
hardware revision produces multiple VCL NALs per frame.

---

## Configuration

Add `sliceRows` and (optionally) `perSliceAu` to `video0` in the JSON config:

```json
"video0": {
  "sliceRows": 4,
  "perSliceAu": false
},
"outgoing": {
  "disablePacketAggregation": false
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

`perSliceAu` (bool, default `false`): when enabled, each H.265 slice is sent
as its own independent RTP access unit with a VPS/SPS/PPS prefix, its own
marker bit, and a fractional RTP timestamp advance.  Requires `sliceRows > 0`.
See **Per-Slice AU Mode** section below.

`disablePacketAggregation` (bool, default `false`): prevent the RTP packetizer
from combining multiple small NALs into a single Aggregation Packet (AP).
Required when `perSliceAu=true`.

---

## Implementation

### Config layer (`include/venc_config.h`, `src/venc_config.c`)

`VencConfigVideo.slice_rows` (int) — parsed from `sliceRows`, clamped to ≥ 0.
`VencConfigVideo.per_slice_au` (bool) — parsed from `perSliceAu`.
`VencConfigOutgoing.disable_packet_aggregation` (bool) — parsed from
`disablePacketAggregation`.

### API layer (`src/venc_api.c`)

Fields with camelCase aliases and `MUT_RESTART` mutation type:

| API field | JSON key |
|---|---|
| `video0.slice_rows` | `video0.sliceRows` |
| `video0.per_slice_au` | `video0.perSliceAu` |
| `outgoing.disable_packet_aggregation` | `outgoing.disablePacketAggregation` |

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

`star6e_hevc_rtp_send_frame` receives both `frame_ticks` and `slice_ticks`:

- **Standard mode** (`per_slice_au=0`): marker bit on last packet of the last
  slice only; timestamp advances by `frame_ticks` once per frame.
- **Per-slice AU mode** (`per_slice_au=1`): each VCL slice is preceded by
  VPS/SPS/PPS, sent with marker bit set, and advances the timestamp by
  `slice_ticks`.  Non-VCL NALs (param sets) that arrive before a slice NAL
  are buffered and flushed with it.

### Video state layer (`include/star6e_video.h`, `src/star6e_video.c`)

`star6e_video_init()` pre-computes `per_slice_au_ticks` from config:

```c
mb_rows    = (height + 15) / 16;
num_slices = (mb_rows + slice_rows - 1) / slice_rows;
per_slice_au_ticks = rtp_frame_ticks / num_slices;
```

This is passed as `slice_ticks` into `star6e_hevc_rtp_send_frame` each
callback.  Computing it from config (not from `count_vcl_nals`) is necessary
because the SigmaStar SDK delivers one slice per callback, so
`count_vcl_nals` would always return 1.

---

## The `endFrame` field

`MI_VENC_Pack_t.endFrame` (char) is set to 1 by the driver on the last pack of
the last slice of a frame, and 0 on intermediate slices. In non-slice mode its
value is unspecified; the code ignores it (`end_of_frame=1` always) in that
case.

---

## Per-Slice AU Mode

### Motivation

Standard H.265 RTP framing puts all slices of a frame into one access unit
sharing a single RTP timestamp.  When any RTP packet is lost the receiver
cannot complete the AU and the whole frame is discarded.

Per-slice AU mode reframes each slice as its own RTP access unit:

```
AU 0: [VPS][SPS][PPS][Slice_0]   ts = T           marker=1
AU 1: [VPS][SPS][PPS][Slice_1]   ts = T + 88      marker=1
AU 2: [VPS][SPS][PPS][Slice_2]   ts = T + 176     marker=1
...
AU 16:[VPS][SPS][PPS][Slice_16]  ts = T + 1408    marker=1
```

A receiver that reassembles these AUs back into a frame can decode all
received slices and conceal only the row-band(s) whose packets were lost.

### RTP timestamp arithmetic

For 1080p at 60 fps with `sliceRows=4`:
- `frame_ticks = 90000 / 60 = 1500`
- `num_slices = ceil(68 / 4) = 17`
- `slice_ticks = 1500 / 17 = 88`

Frame boundaries are always at multiples of `frame_ticks` from the anchor.
The slice index within a frame is `round((ts − frame_base) / slice_ticks)`.
This arithmetic is used by the reassembler to assign each AU to its
frame and slice slot without parsing H.265 headers.

### Required encoder settings

```json
"video0": { "sliceRows": 4, "perSliceAu": true },
"outgoing": { "disablePacketAggregation": true }
```

`disablePacketAggregation` is required so each NAL occupies its own RTP
packet and the per-slice AU framing is not disturbed by AP bundling.

---

## Decode

venc uses **PT=97 for H.265** (PT=96 is H.264). Use `payload=97` in all
GStreamer caps. Using `payload=96` causes `rtph265depay` to silently discard
every packet.

### Standard pipeline (perSliceAu=false)

When slices are enabled but `perSliceAu=false`, all slices share one RTP
timestamp.  `rtph265depay` accumulates NALs until the marker bit arrives, then
`h265parse` assembles them into one AU.

```
gst-launch-1.0 udpsrc address=127.0.0.1 port=5600 buffer-size=65536 \
  caps="application/x-rtp,encoding-name=H265,payload=97,clock-rate=90000" \
! rtpjitterbuffer latency=20 do-lost=true drop-on-latency=true \
! rtph265depay \
! "video/x-h265,stream-format=byte-stream,alignment=nal" \
! h265parse \
! vah265dec \
! autovideosink sync=false
```

**Use `vah265dec`, not `avdec_h265`**, for error resilience. `avdec_h265`
uses motion-vector concealment on a corrupt slice and then uses that corrupted
frame as a reference for subsequent P-frames, spreading artefacts across the
entire GOP. `vah265dec` freezes on the first corrupt frame and keeps the last
clean frame as reference — one glitch, then immediately clean.

To limit the maximum corruption window from packet loss, reduce `gopSize`:

| gopSize | Max corruption frames @ 60 fps | Bitrate impact |
|---------|-------------------------------|----------------|
| 1.0 s   | ~60                           | baseline       |
| 0.5 s   | ~30                           | +5–8 %         |
| 0.25 s  | ~15                           | +12–18 %       |
| 0.1 s   | ~6                            | +25–40 %       |

0.25–0.5 s is a typical FPV sweet spot.

### Per-slice AU pipeline (perSliceAu=true)

Standard RTP pipelines (`rtpjitterbuffer` → `rtph265depay`) flush their entire
frame buffer on a packet-loss gap event, turning any single lost packet into a
full dropped frame regardless of which slice was hit.

The reassembler in `tools/hevc_slice_reassembler.py` bypasses this by receiving
raw RTP and doing its own frame assembly:

1. Groups per-slice AUs into frames using timestamp arithmetic
   (`frame_index = (ts − anchor) / frame_ticks`).
2. Accumulates slice NALs into per-frame buffers.
3. After one frame period, emits whatever slices arrived as one H.265
   Annex-B AU to a GStreamer `appsrc`.
4. Lost slices appear as gaps in the AU; `vah265dec` decodes what it has
   and freezes only the affected row-band(s).

```bash
python3 tools/hevc_slice_reassembler.py \
    --addr 127.0.0.1 --port 5600 \
    --fps 60 --height 1080 --slice-rows 4 \
    --decoder vah265dec
```

The script accepts `--decoder avdec_h265` as a fallback if VA-API is
unavailable, but the row-band isolation benefit is reduced because `avdec_h265`
propagates concealment errors across P-frames.

**Lost first-slice edge case**: if the first slice of a frame (`slice_index=0`)
is lost, the assembled AU would start with a non-first slice
(`first_slice_segment_in_pic_flag=0`), which is invalid H.265. The reassembler
detects this and skips the frame, letting `vah265dec` freeze the full frame for
that interval. With 17 slices this affects ~1/17 ≈ 6% of loss events; the
other 94% result in partial-row concealment only.

---

## Limitations

- WebUI does not expose `sliceRows`, `perSliceAu`, or `disablePacketAggregation`
  (the WebUI dashboard is a pre-compiled gzip blob with no source in this
  repository).
- H264 slice split (`fnSetH264SliceSplit`) is wired but untested.
- The dual-stream ch1 (recording channel) always runs without slices.
- The firmware `MI_VENC_SetH265SliceSplit` symbol may be absent on older
  builds; in that case slicing is silently disabled and full-frame mode is used.
