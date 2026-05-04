# Per-Slice AU Reassembler

## Problem

H.265 encodes a frame in one or more independent slices.  Each slice covers a
horizontal band of macroblocks and can be decoded without the other slices
from the same frame.  This property should, in theory, allow a lost RTP packet
to corrupt only the row-band it covered and leave the rest of the frame clean.

In practice, standard RTP receive pipelines destroy this property.
`rtpjitterbuffer` tracks RTP sequence numbers and emits a gap event the moment
it detects a missing packet in the sequence.  `rtph265depay` responds to that
event by flushing its internal NAL accumulator and injecting a GStreamer
discontinuity downstream.  `h265parse` sees the discontinuity and also flushes.
The net result is that the entire frame's worth of NALs is discarded — even
the slices whose packets arrived perfectly — and the decoder receives nothing
for that frame.

```
Standard pipeline on loss:

  Packet 1 ──▶ ┐
  Packet 2 ──▶ │  rtpjitterbuffer  ──▶  rtph265depay ──▶ h265parse ──▶ decoder
  [LOST]       │  detects gap               flushes        flushes     receives
  Packet 4 ──▶ ┘  emits GAP event       all NALs lost   all NALs    nothing
                                                                    ─────────
                                                          full frame lost
```

The reassembler replaces `rtpjitterbuffer` + `rtph265depay` with its own
receive path that does not flush on loss.  Whatever slices arrive are kept;
the H.265 decoder receives a partial but structurally valid frame and applies
error concealment only to the missing row-bands.

```
Reassembler on loss:

  Packet 1 ──▶ ┐                                         slice 0 ✓
  Packet 2 ──▶ │  raw UDP recv  ──▶  reassembler ──▶ h265parse ──▶ decoder
  [LOST]       │  no gap events     collects what     reassembled   receives
  Packet 4 ──▶ ┘  no flushes       arrived; emits    AU with gap   partial frame
                                    partial AU                    ─────────────
                                                         only band N frozen
```

---

## Encoder side: per-slice AU mode

For the reassembler to work, the encoder must send each slice as its own
independent RTP access unit.  This is `perSliceAu=true` in venc config.

In standard slice mode (`perSliceAu=false`) all slices of a frame share one
RTP timestamp.  `rtph265depay` accumulates them until the marker bit on the
last one, then outputs them all as a single buffer.  There is no way to
separate received from lost slices at that point.

In per-slice AU mode each slice is its own AU:

```
AU 0:  [VPS][SPS][PPS][Slice_0]   ts = T           marker=1
AU 1:  [VPS][SPS][PPS][Slice_1]   ts = T + 88      marker=1
AU 2:  [VPS][SPS][PPS][Slice_2]   ts = T + 176     marker=1
...
AU 16: [VPS][SPS][PPS][Slice_16]  ts = T + 1408    marker=1
```

Each AU has its own RTP timestamp and its own marker bit.  The reassembler can
tell which frame and which row-band each AU belongs to purely from the timestamp,
with no H.265 header parsing.

---

## Timestamp arithmetic

The RTP clock runs at 90 000 Hz.  For a 60 fps stream:

```
frame_ticks  = 90000 / 60  = 1500
```

For 1080p with `sliceRows=4`:

```
mb_rows      = ceil(1080 / 16) = 68
num_slices   = ceil(68 / 4)    = 17
slice_ticks  = 1500 / 17       = 88   (integer division)
```

From any per-slice AU's RTP timestamp `ts`:

```
frame_index  = (ts - anchor) / frame_ticks          integer division
slice_index  = round((ts - frame_base) / slice_ticks)
```

where `frame_base = anchor + frame_index * frame_ticks` and `anchor` is
snapped to the nearest frame boundary when the first packet arrives:

```python
anchor = ts - (ts % frame_ticks)
```

This arithmetic maps every incoming RTP packet to its exact `(frame, slice)`
slot without touching the H.265 bitstream.  Both computations use 32-bit
modular arithmetic to handle the RTP timestamp rollover at 2³² ticks
(approximately 13 hours at 90 kHz).

---

## Implementation

The reassembler is `tools/hevc_slice_reassembler.py`.  It has four layers.

### Layer 1: RTP parsing (`rtp_parse`)

Parses the 12-byte fixed RTP header plus any CSRC and extension bytes to
extract `(seq, ts, ssrc, payload)`.  Rejects packets with version ≠ 2.
The SSRC is not used for filtering here — the encoder sends a single video
SSRC, so all packets on the port belong to the same stream.

### Layer 2: HEVC depayloading

Three RFC 7798 packet types are handled:

**Single NAL unit** (type 0–47 excluding 48 and 49): the RTP payload is the
NAL unit verbatim.  Passed straight to the frame accumulator.

**FU — Fragmentation Unit** (type 49): used when a NAL is too large for one
RTP packet.  The `FuBuf` class reassembles fragments keyed by `(ssrc, ts)`.
Each FU packet carries a 1-byte FU header after the 2-byte outer NAL header:

```
bit 7: S (start of NAL)
bit 6: E (end of NAL)
bits 5–0: FuType (original NAL type)
```

On the start fragment, `FuBuf` reconstructs the 2-byte original NAL header:

```python
byte0 = (fu_type << 1) | (outer_hdr[0] & 0x01)   # type + layer_id MSB
byte1 = outer_hdr[1]                               # layer_id LSBs + TID
```

Middle and end fragments append their payload.  When `E=1`, the NAL is
complete and passed to the frame accumulator.

**AP — Aggregation Packet** (type 48): disabled by `disablePacketAggregation`
on the encoder, but the reassembler handles it defensively by walking the
length-prefixed NALU list inside the AP payload.

### Layer 3: frame accumulation (`Frame`)

Each frame index maps to a `Frame` object holding:

- `slices`: dict of `slice_index → NAL bytes` for VCL NALs (type 0–31)
- `vps`, `sps`, `pps`: the most recent parameter set NALs seen for this frame
- `last_rx`: monotonic time of the most recent slice packet, used for timeout

NAL type is determined from the two-byte HEVC header: `(byte0 >> 1) & 0x3F`.
Types 0–31 are VCL (slice data).  Types 32–34 are VPS, SPS, PPS.

The `Reassembler` class manages the dict of active `Frame` objects.  Only one
flush path is used:

**Timeout flush** (`tick`): a GLib timer fires every 20 ms and flushes any
frame whose `last_rx` (time of last VCL NAL received) is older than 400 ms.
This 400 ms window comfortably exceeds the observed WFB-NG FEC delivery spread
of ~300 ms per frame.  An arrival-triggered flush (on seeing a higher frame
index) was considered but rejected: WFB-NG delivers packets from multiple
frames simultaneously so a higher fidx arriving does not mean the lower frame
is complete.

### Layer 4: Annex-B assembly and GStreamer output (`_flush`)

When a frame is flushed, `Frame.assemble()` builds a complete Annex-B
bytestream:

```
\x00\x00\x00\x01  [VPS]
\x00\x00\x00\x01  [SPS]
\x00\x00\x00\x01  [PPS]
\x00\x00\x00\x01  [Slice_0]
\x00\x00\x00\x01  [Slice_1]        ← absent if slice 1 was lost
\x00\x00\x00\x01  [Slice_2]
...
```

If this frame did not carry its own VPS/SPS/PPS (unusual in per-slice AU
mode, but handled for robustness), the most recently seen param sets from
a previous frame are used as fallback.

The bytestream is wrapped in a `Gst.Buffer` with a monotonically increasing
PTS (`pts_ns`, advancing by `1e9 / fps` per frame regardless of whether
some frames were skipped) and pushed to the GStreamer `appsrc`.

The downstream pipeline is:

```
appsrc (alignment=au) → h265parse → vah265dec → autovideosink
```

`appsrc` declares `alignment=au` so `h265parse` knows each buffer is already
a complete access unit.  `h265parse` still adds value: it negotiates codec
caps (resolution, profile, level) from the SPS before the first frame reaches
the decoder.  `vah265dec` is strongly preferred over `avdec_h265` because its
error behaviour on a partial frame is to freeze the output rather than attempt
motion-vector concealment — see below.

---

## Decoder error behaviour

When the decoder receives a frame with one or more slices missing, it must
decide what to display for the absent row-bands.  The two common H.265
software decoders on Linux handle this very differently.

**`vah265dec` (VA-API hardware decoder)**

Sends the partial frame to the hardware and accepts whatever the hardware
produces.  For missing slice regions the hardware typically copies the
corresponding row-band from the previous decoded frame (freeze concealment).
The frozen band appears for exactly one frame interval, after which the next
complete or partial frame overwrites it.  Because the frozen pixels came from
the previously decoded reference frame rather than a synthetic estimate, the
reference frame used for subsequent P-frame prediction is also clean.  Error
does not propagate.

**`avdec_h265` (FFmpeg software decoder)**

Attempts active error concealment by extrapolating motion vectors into the
missing region.  This produces a blurred or smeared approximation of the
missing content, and — critically — that synthesised content becomes the
reference frame for subsequent P-frames.  Any inaccuracy in the concealment
compounds over the rest of the GOP (up to `gopSize` seconds at 60 fps).
What starts as one missing slice band can spread into a gradually worsening
artefact across the entire frame for tens of frames.

Use `vah265dec`.  Fall back to `avdec_h265` only if VA-API is unavailable,
and reduce `gopSize` to limit the corruption window.

---

## Edge cases

### Lost first slice

The first slice of a frame has `first_slice_segment_in_pic_flag=1` in the
H.265 bitstream.  A frame AU that does not begin with this slice is not a
valid H.265 bitstream — `h265parse` or the decoder will reject it.

When `slice_index=0` is absent from the accumulated `Frame`, `assemble()`
returns `(None, [])`.  `_flush()` detects this, advances the PTS by one
frame interval (to avoid a PTS gap stall), prints a diagnostic line, and
emits nothing to the decoder.  `vah265dec` will freeze the full frame for
that interval and recover on the next frame.

With 17 slices, the first-slice AU is hit by roughly 1/17 ≈ 6% of
single-packet loss events.  The other 94% result in partial-row concealment.

### Encoder timestamp drift (integer-division remainder)

In per-slice AU mode the encoder advances the RTP timestamp by `slice_ticks`
after each VCL NAL.  With `num_slices=17` and `frame_ticks=1500`:

```
slice_ticks = 1500 // 17 = 88      (integer division)
17 × 88     = 1496 ≠ 1500
remainder   = 4 ticks per frame
```

Without correction the next frame starts 4 ticks too early.  After 8 frames
the drift is 32 ticks; the first slice of frame 8 lands 32 ticks below the
reassembler's expected frame boundary and is assigned `slice_index=17`
(out of range, discarded).  "First slice lost" then appears for every frame
from that point on.

The encoder fixes this by snapping `rtp->timestamp = frame_start_ts +
frame_ticks` at the end of each per-slice AU frame instead of letting the
accumulated `+slice_ticks` increments run short.

### FU loss mid-fragment

If a middle or end FU packet is lost, `FuBuf.done` never becomes `True` and
the FU buffer is never passed to the frame accumulator.  The incomplete `FuBuf`
object stays in `self.fus` and is garbage-collected when the next FU for a
different `(ssrc, ts)` key displaces it.  The slice is treated as absent and
the frame is emitted with that slice missing.

If the start FU packet is lost, no `FuBuf` is created for that `(ssrc, ts)`
key, so middle and end packets are silently dropped.  Same outcome: the slice
is absent.

### RTP sequence reordering

The reassembler does not reorder RTP packets.  For FU packets, arrival out of
order would produce a corrupted NAL.  In practice, on a local wired or managed
wireless link, UDP datagrams from a single source arrive in order.  Over a
WFB-NG link, the FEC layer reassembles before delivery so reordering is also
rare.  If reordering is a concern, the FuBuf could be extended to sort by
sequence number before assembly.

### RTP timestamp rollover

All timestamp comparisons use `& 0xFFFFFFFF` masking and unsigned subtraction,
matching the 32-bit modular arithmetic of the RTP spec.  Rollover at 2³² ticks
(≈13 hours at 90 kHz) is handled transparently.

---

## Limitations

- **SSC338Q/SSC30KQ hardware does not produce multiple slices**: tested
  firmware returns only 1 VCL NAL per frame even when `sliceRows=4` and
  `MI_VENC_SetH265SliceSplit` returns 0.  With 1 slice/frame the reassembler
  receives 1 AU/frame; a lost packet freezes the entire frame rather than a
  single row-band.  The reassembler is correct and will provide row-band
  isolation as soon as a firmware or hardware revision delivers multiple VCL
  NALs per frame.  See `documentation/SLICES.md` — Hardware Status for details.

- **No H.265 header parsing**: slice type, quantiser step, and row position
  within the frame are not inspected.  Missing slices are identified by absent
  slice index slots, not by `slice_segment_address`.  If the encoder changes
  `sliceRows` at runtime the reassembler must be restarted with updated
  `--slice-rows`.
- **Config must match encoder**: `--fps`, `--height`, and `--slice-rows` must
  exactly match the running encoder config.  A mismatch produces wrong frame
  and slice indices, causing frames to be assembled incorrectly or dropped.
- **Single SSRC stream**: the reassembler assumes all packets on the UDP port
  belong to one video stream.  Audio on the same port (avoid: see
  `audioPort` config) would produce garbage NALs.
- **Python GIL and 60 fps**: the rx thread and the GLib timer share the same
  lock.  At 60 fps with 17 slices the arrival rate is ~1020 packets/s.  This
  is well within Python's capability for UDP receive, but on very slow hardware
  a C implementation may be necessary.

---

## Usage

```bash
# 1080p / 60fps / sliceRows=4 on loopback
python3 tools/hevc_slice_reassembler.py \
    --addr 127.0.0.1 --port 5600 \
    --fps 60 --height 1080 --slice-rows 4 \
    --decoder vah265dec

# Software fallback (no VA-API)
python3 tools/hevc_slice_reassembler.py \
    --addr 127.0.0.1 --port 5600 \
    --fps 60 --height 1080 --slice-rows 4 \
    --decoder avdec_h265
```

Required encoder config:

```json
"video0": { "sliceRows": 4, "perSliceAu": true },
"outgoing": { "disablePacketAggregation": true }
```

On loss, the script prints the pixel row range of each missing band:

```
[frame 4217] 1/17 slices missing: rows ['128–191px']
[frame 4218] 2/17 slices missing: rows ['64–127px', '192–255px']
[frame 4301] first slice lost — skipping (decoder will freeze this frame)
```

At exit, a summary is printed:

```
── Stats ────────────────────────────────────────
  Frames emitted : 3600
  Partial frames : 12
  Skipped (no first slice): 1
```
