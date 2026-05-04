#!/usr/bin/env python3
"""
rtp_slice_check.py — RTP packet analyzer for confirming H265 slice split

Parses RTP payloads (RFC 7798) to count VCL NAL units per timestamp group and
detect slice split in both encoder modes:

  Standard mode (perSliceAu=false):
    All slices of one frame share one RTP timestamp.  Multiple VCL NALs appear
    in the same (ssrc, ts) group.  vcl_per_group is the slice count.

  Per-slice AU mode (perSliceAu=true):
    Each slice has its own RTP timestamp, ~slice_ticks apart.  One VCL NAL per
    timestamp group, but N_slices groups per frame interval.
    vcl_per_frame = vcl_per_group × groups_per_frame.

Also parses H.265 slice headers directly from the bitstream to read
first_slice_segment_in_pic_flag and slice_segment_address, providing
bitstream-level proof of multi-slice encoding independent of RTP packaging.

  If the encoder produces multiple slices per picture (real spatial partitioning):
    • Multiple VCL NALs appear with the same RTP timestamp (standard mode)
    • Non-first slices have first_slice_segment_in_pic_flag = 0
    • slice_segment_address increases: 0, step, 2*step, ... (CTB raster units)

Stream separation is done by SSRC so audio on the same port is excluded.
Packets within each group are sorted by RTP sequence number.

Usage:
    python3 rtp_slice_check.py [PORT [N_PACKETS [FPS]]]

    PORT        UDP port to listen on (default: 5600)
    N_PACKETS   Number of RTP packets to capture (default: 500)
    FPS         Encoder frame rate — used for per-slice AU detection (default: 60)

Exit code 0 = slices confirmed, 1 = no slices detected or no packets received.
"""
import math
import socket
import struct
import sys
from collections import Counter, OrderedDict, defaultdict

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5600
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 500
FPS  = int(sys.argv[3]) if len(sys.argv) > 3 else 60

RTP_CLOCK   = 90000
FRAME_TICKS = RTP_CLOCK // FPS

HEVC_FU       = 49
HEVC_AP       = 48
HEVC_SPS_TYPE = 33
HEVC_IRAP     = set(range(16, 24))   # NAL types 16-23 are IRAP (IDR/BLA/CRA)


# ── Bit reader with emulation-prevention-byte removal ───────────────────────

class BitReader:
    """Read individual bits from an H.265 RBSP byte string."""

    def __init__(self, data):
        self._data = bytearray(self._strip_ep(bytes(data)))
        self._pos = 0

    @staticmethod
    def _strip_ep(data):
        out = bytearray()
        i = 0
        while i < len(data):
            if (i + 2 < len(data) and
                    data[i] == 0 and data[i+1] == 0 and data[i+2] == 3):
                out += b'\x00\x00'
                i += 3
            else:
                out.append(data[i])
                i += 1
        return out

    def read_bit(self):
        if self._pos >= len(self._data) * 8:
            raise EOFError
        b   = self._data[self._pos >> 3]
        bit = (b >> (7 - (self._pos & 7))) & 1
        self._pos += 1
        return bit

    def read_bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.read_bit()
        return v

    def read_ue(self):
        lz = 0
        while self.read_bit() == 0:
            lz += 1
            if lz > 31:
                raise ValueError("ue(v): leading zeros > 31")
        v = (1 << lz) - 1
        if lz:
            v += self.read_bits(lz)
        return v


# ── H.265 SPS parser (H.265 §7.3.2.2) ──────────────────────────────────────

def _skip_ptl(br, profile_present, max_sub_layers_minus1):
    """Skip profile_tier_level() to reach fields after it in the SPS."""
    if profile_present:
        br.read_bits(2 + 1 + 5)   # profile_space, tier_flag, profile_idc
        br.read_bits(32)           # profile_compatibility_flag[32]
        br.read_bits(4)            # progressive/interlaced/non-packed/frame-only
        br.read_bits(44)           # constraint flags (reserved_zero_44bits, Main/Main10)
        br.read_bits(8)            # level_idc
    sub_prof = []
    sub_lev  = []
    for _ in range(max_sub_layers_minus1):
        sub_prof.append(br.read_bit())
        sub_lev.append(br.read_bit())
    if max_sub_layers_minus1 > 0:
        for _ in range(max_sub_layers_minus1, 8):
            br.read_bits(2)         # reserved_zero_2bits
    for i in range(max_sub_layers_minus1):
        if sub_prof[i]:
            br.read_bits(2 + 1 + 5 + 32 + 4 + 44 + 8)
        if sub_lev[i]:
            br.read_bits(8)


def parse_sps(nal):
    """
    Parse an H.265 SPS NAL unit (2-byte NAL header included).
    Returns a dict with CTB geometry, or None on parse failure.

      width, height     — picture dimensions in luma samples
      ctb_size          — CTB size in pixels (power of 2, usually 64)
      ctbs_w, ctbs_h    — picture width/height in CTBs
      num_ctbs          — PicSizeInCtbsY = ctbs_w × ctbs_h
      addr_bits         — bit width of slice_segment_address field
    """
    try:
        br = BitReader(nal[2:])
        br.read_bits(4)            # sps_video_parameter_set_id
        max_sub = br.read_bits(3)  # sps_max_sub_layers_minus1
        br.read_bit()              # sps_temporal_id_nesting_flag
        _skip_ptl(br, True, max_sub)
        br.read_ue()               # sps_seq_parameter_set_id
        chroma = br.read_ue()
        if chroma == 3:
            br.read_bit()          # separate_colour_plane_flag
        width  = br.read_ue()      # pic_width_in_luma_samples
        height = br.read_ue()      # pic_height_in_luma_samples
        if br.read_bit():          # conformance_window_flag
            br.read_ue(); br.read_ue(); br.read_ue(); br.read_ue()
        br.read_ue()               # bit_depth_luma_minus8
        br.read_ue()               # bit_depth_chroma_minus8
        br.read_ue()               # log2_max_pic_order_cnt_lsb_minus4
        sub_ord = br.read_bit()
        start = 0 if sub_ord else max_sub
        for _ in range(start, max_sub + 1):
            br.read_ue(); br.read_ue(); br.read_ue()
        log2_min_cb = br.read_ue()  # log2_min_luma_coding_block_size_minus3
        log2_diff   = br.read_ue()  # log2_diff_max_min_luma_coding_block_size
        ctb_log2 = (log2_min_cb + 3) + log2_diff
        ctb_size = 1 << ctb_log2
        pw = math.ceil(width  / ctb_size)
        ph = math.ceil(height / ctb_size)
        num_ctbs  = pw * ph
        addr_bits = max(1, math.ceil(math.log2(num_ctbs))) if num_ctbs > 1 else 1
        return dict(width=width, height=height, ctb_size=ctb_size,
                    ctbs_w=pw, ctbs_h=ph, num_ctbs=num_ctbs,
                    addr_bits=addr_bits)
    except Exception:
        return None


# ── H.265 slice header parser (H.265 §7.3.6) ────────────────────────────────

def parse_slice_hdr(nal, sps):
    """
    Parse an H.265 slice segment header (2-byte NAL header included).
    Returns dict(first, addr) or None on failure.

      first  True  → first_slice_segment_in_pic_flag = 1 (first slice of picture)
             False → subsequent slice; addr is slice_segment_address in CTBs
      addr   0 for first slice; CTB raster address for subsequent slices
             None if addr_bits could not be determined (no SPS)
    """
    if len(nal) < 3:
        return None
    try:
        nal_type = (nal[0] >> 1) & 0x3F
        br = BitReader(nal[2:])
        first = bool(br.read_bit())       # first_slice_segment_in_pic_flag
        if nal_type in HEVC_IRAP:
            br.read_bit()                 # no_output_of_prior_pics_flag
        br.read_ue()                      # slice_pic_parameter_set_id
        if first:
            return dict(first=True, addr=0)
        # dependent_slice_segment_flag: skip if PPS dependent_slice_segments_enabled_flag=0
        # (we assume 0 — the SigmaStar encoder does not use dependent slices)
        addr = None
        if sps and sps['num_ctbs'] > 1:
            addr = br.read_bits(sps['addr_bits'])
        return dict(first=False, addr=addr)
    except Exception:
        return None


# ── NAL extraction helpers ───────────────────────────────────────────────────

def count_vcl_starts(payload):
    """Count VCL NAL unit starts in one RTP packet payload (RFC 7798).

    Returns the number of new VCL (slice) NAL units that begin in this packet.
    Non-VCL types (VPS/SPS/PPS/SEI = 32+) and mid/end FU fragments count as 0.
    """
    if len(payload) < 2:
        return 0
    nt = (payload[0] >> 1) & 0x3F
    if nt == HEVC_FU:
        # FU header byte is at offset 2; S bit (0x80) marks start of a new NAL.
        if len(payload) < 3 or not (payload[2] & 0x80):
            return 0
        return 1 if (payload[2] & 0x3F) <= 31 else 0
    if nt == HEVC_AP:
        # Walk length-prefixed NALU list starting after the 2-byte AP header.
        count = 0
        off = 2
        while off + 2 <= len(payload):
            nlen = (payload[off] << 8) | payload[off + 1]
            off += 2
            if off + nlen > len(payload):
                break
            if len(payload) > off and ((payload[off] >> 1) & 0x3F) <= 31:
                count += 1
            off += nlen
        return count
    return 1 if nt <= 31 else 0


def extract_vcl_headers(payload):
    """
    Return list of (nal_type, bytes) for each VCL NAL start in this RTP packet.
    For FU start packets the 2-byte NAL header is reconstructed from the FU
    header fields so that parse_slice_hdr can process the result directly.
    Only the first 64 bytes of each NAL (including the 2-byte header) are kept —
    enough to parse first_slice_segment_in_pic_flag and slice_segment_address.
    """
    out = []
    if len(payload) < 2:
        return out
    nt = (payload[0] >> 1) & 0x3F
    if nt == HEVC_FU:
        if len(payload) >= 3 and (payload[2] & 0x80):   # S-bit set
            fu_type = payload[2] & 0x3F
            if fu_type <= 31:
                h0 = (fu_type << 1) | (payload[0] & 0x01)
                h1 = payload[1]
                out.append((fu_type, bytes([h0, h1]) + bytes(payload[3:67])))
    elif nt == HEVC_AP:
        off = 2
        while off + 2 <= len(payload):
            nlen = (payload[off] << 8) | payload[off + 1]
            off += 2
            if off + nlen > len(payload):
                break
            sub_nt = (payload[off] >> 1) & 0x3F if off < len(payload) else 0xFF
            if sub_nt <= 31:
                out.append((sub_nt, bytes(payload[off:off + min(nlen, 64)])))
            off += nlen
    elif nt <= 31:
        out.append((nt, bytes(payload[:64])))
    return out


def extract_sps_nal(payload):
    """Return the SPS NAL bytes from this RTP packet, or None."""
    if len(payload) < 2:
        return None
    nt = (payload[0] >> 1) & 0x3F
    if nt == HEVC_SPS_TYPE:
        return bytes(payload)
    if nt == HEVC_AP:
        off = 2
        while off + 2 <= len(payload):
            nlen = (payload[off] << 8) | payload[off + 1]
            off += 2
            if off + nlen > len(payload):
                break
            sub = payload[off:off + nlen]
            if sub and (sub[0] >> 1) & 0x3F == HEVC_SPS_TYPE:
                return bytes(sub)
            off += nlen
    return None


# ── Capture ──────────────────────────────────────────────────────────────────

print(f"Listening on 0.0.0.0:{PORT}, capturing {N} RTP packets "
      f"(fps={FPS}, frame_ticks={FRAME_TICKS})...", flush=True)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(10.0)

groups         = defaultdict(list)   # (ssrc, ts) → [(seq, marker), ...]
vcl_counts     = defaultdict(int)    # (ssrc, ts) → VCL NAL starts
vcl_hdrs       = defaultdict(list)   # (ssrc, ts) → [(nal_type, bytes)]
sps_list       = []                  # up to 5 captured SPS NAL payloads
ssrc_pkt_count = Counter()
count = 0

try:
    while count < N:
        data, _ = sock.recvfrom(2048)
        if len(data) < 12 or (data[0] >> 6) != 2:
            continue
        cc      = data[0] & 0x0F
        has_ext = (data[0] >> 4) & 1
        marker  = bool(data[1] & 0x80)
        seq, ts, ssrc = struct.unpack_from(">HII", data, 2)
        off = 12 + cc * 4
        if has_ext and len(data) >= off + 4:
            off += 4 + (struct.unpack_from(">H", data, off + 2)[0]) * 4
        payload = data[off:]

        groups[(ssrc, ts)].append((seq, marker))
        vcl_counts[(ssrc, ts)] += count_vcl_starts(payload)
        ssrc_pkt_count[ssrc] += 1

        for item in extract_vcl_headers(payload):
            vcl_hdrs[(ssrc, ts)].append(item)

        sps = extract_sps_nal(payload)
        if sps and len(sps_list) < 5:
            sps_list.append(sps)

        count += 1
        if count % 50 == 0:
            print(f"  {count}/{N} packets received...", flush=True)
except socket.timeout:
    print(f"Timeout after {count} packets")

sock.close()

if not groups:
    print("\nNo packets received.")
    sys.exit(1)

for key in groups:
    groups[key].sort(key=lambda p: p[0])

# ── Per-SSRC breakdown ───────────────────────────────────────────────────────

ssrc_groups = {}
for (ssrc, ts), pkts in groups.items():
    ssrc_groups.setdefault(ssrc, OrderedDict())[ts] = pkts

def avg_group_size(g):
    sizes = [len(v) for v in g.values()]
    return sum(sizes) / len(sizes) if sizes else 0

ssrcs      = list(ssrc_groups.keys())
video_ssrc = max(ssrc_groups, key=lambda s: avg_group_size(ssrc_groups[s]))

print(f"\n--- Results: {count} packets, {len(groups)} timestamp groups, "
      f"{len(ssrcs)} SSRC(s) ---")

for ssrc, g in ssrc_groups.items():
    label  = "VIDEO" if ssrc == video_ssrc else "other"
    sizes  = [len(v) for v in g.values()]
    avg    = sum(sizes) / len(sizes) if sizes else 0
    ts_vals = sorted(g.keys())
    if len(ts_vals) >= 2:
        deltas = [(ts_vals[i+1] - ts_vals[i]) & 0xFFFFFFFF
                  for i in range(len(ts_vals) - 1)]
        cd     = Counter(deltas).most_common(1)[0][0]
        fps_str = f"{RTP_CLOCK / cd:.1f} step-fps"
    else:
        fps_str = "?"
    print(f"  SSRC 0x{ssrc:08X}  [{label}]  "
          f"{ssrc_pkt_count[ssrc]:4d} pkts  {len(g):3d} groups  "
          f"avg {avg:.1f} pkts/group  ~{fps_str}")

# ── Video stream analysis ────────────────────────────────────────────────────

vg     = ssrc_groups[video_ssrc]
v_ts   = sorted(vg.keys())
v_pkts = [len(vg[ts]) for ts in v_ts]
v_vcl  = [vcl_counts[(video_ssrc, ts)] for ts in v_ts]

if len(v_ts) >= 2:
    deltas       = [(v_ts[i+1] - v_ts[i]) & 0xFFFFFFFF
                    for i in range(len(v_ts) - 1)]
    common_delta = Counter(deltas).most_common(1)[0][0]
else:
    common_delta = FRAME_TICKS

# Per-slice AU mode: timestamp step is slice_ticks << frame_ticks.
per_slice_au = common_delta < FRAME_TICKS // 2

print(f"\n── Video groups (SSRC 0x{video_ssrc:08X}) — first 20 ─────────────────────")
print(f"  {'Timestamp':>12}  {'Pkts':>5}  {'VCLs':>5}  Marker at")
for i, ts in enumerate(v_ts):
    if i >= 20:
        print(f"  ... ({len(v_ts) - 20} more)")
        break
    pkts   = vg[ts]
    vcl    = vcl_counts[(video_ssrc, ts)]
    marked = [j for j, (_, m) in enumerate(pkts) if m]
    print(f"  {ts:>12}  {len(pkts):>5}  {vcl:>5}  "
          f"{marked if marked else '—'}")

# ── VCL NAL distribution ─────────────────────────────────────────────────────

print(f"\n── VCL NALs per timestamp group ─────────────────────────────────")
vcl_dist = Counter(v_vcl)
max_vc   = max(vcl_dist.values())
for n in sorted(vcl_dist):
    bar = "█" * int(round(vcl_dist[n] / max_vc * 20))
    print(f"  {n:3d} VCL/group  x{vcl_dist[n]:4d}  {bar}")
avg_vcl_per_group = sum(v_vcl) / len(v_vcl) if v_vcl else 0.0

# ── Mode detection ───────────────────────────────────────────────────────────

print(f"\n── Mode detection ───────────────────────────────────────────────")
if per_slice_au:
    n_slices_est  = max(1, round(FRAME_TICKS / common_delta))
    n_frames_est  = len(v_ts) / n_slices_est
    vcl_per_frame = (sum(v_vcl) / n_frames_est) if n_frames_est > 0 else 0.0
    print(f"  Per-slice AU  (slice_ticks≈{common_delta}, frame_ticks={FRAME_TICKS})")
    print(f"  ~{n_slices_est} slices/frame estimated from timestamp step")
    print(f"  {len(v_ts)} groups / {n_frames_est:.1f} frames → "
          f"{len(v_ts)/n_frames_est:.1f} groups/frame")
    print(f"  Avg VCL/group: {avg_vcl_per_group:.2f} → "
          f"{vcl_per_frame:.1f} VCL/frame")
else:
    vcl_per_frame = avg_vcl_per_group
    print(f"  Standard mode  (one timestamp per frame, delta={common_delta})")
    print(f"  Avg VCL/group: {avg_vcl_per_group:.2f}  →  {vcl_per_frame:.1f} VCL/frame")

# ── Packets per group distribution ───────────────────────────────────────────

print(f"\n── Packets per timestamp group ──────────────────────────────────")
dist  = Counter(v_pkts)
max_c = max(dist.values())
for size in sorted(dist):
    bar = "█" * int(round(dist[size] / max_c * 20))
    print(f"  {size:3d} pkt/group  x{dist[size]:4d}  {bar}")
print(f"\n  Min {min(v_pkts)}  /  Avg {sum(v_pkts)/len(v_pkts):.1f}  /  "
      f"Max {max(v_pkts)}  ({len(v_pkts)} groups)")

# ── Marker-bit check ─────────────────────────────────────────────────────────

ok = bad = no_marker = 0
for ts in v_ts:
    pkts   = vg[ts]
    marked = [j for j, (_, m) in enumerate(pkts) if m]
    if not marked:
        no_marker += 1
    elif marked == [len(pkts) - 1]:
        ok += 1
    else:
        bad += 1

print(f"\n── Marker-bit check (RFC 7798) ──────────────────────────────────")
print(f"  Marker on last packet only : {ok}")
if no_marker:
    print(f"  No marker (partial/cutoff) : {no_marker}")
print(f"  Marker misplaced           : {bad}"
      + ("  ← PROBLEM" if bad else "  ✓"))

# ── Slice header analysis ─────────────────────────────────────────────────────

print(f"\n── Slice header analysis (H.265 bitstream) ──────────────────────")

# Parse SPS to determine CTB geometry and slice_segment_address bit width.
sps_info = None
for sps_nal in sps_list:
    info = parse_sps(sps_nal)
    if info:
        sps_info = info
        break

if sps_info:
    print(f"  SPS decoded: {sps_info['width']}×{sps_info['height']} px  "
          f"CTB={sps_info['ctb_size']}px  "
          f"{sps_info['ctbs_w']}×{sps_info['ctbs_h']}={sps_info['num_ctbs']} CTBs  "
          f"addr field={sps_info['addr_bits']} bits")
else:
    print("  SPS: not captured — slice_segment_address will show as '?'")

# Parse slice headers for the first 12 timestamp groups of the video SSRC.
# In standard mode (perSliceAu=false) each group is one frame; multi-slice
# frames will show multiple VCL entries with distinct addresses.
sample_ts = v_ts[:min(12, len(v_ts))]

all_addrs        = set()   # all non-zero slice_segment_address values seen
max_vcl_any      = 0       # max VCL NALs in any single group
groups_with_multi = 0      # groups that had > 1 VCL NAL

for ts in sample_ts:
    hdrs = vcl_hdrs.get((video_ssrc, ts), [])
    if not hdrs:
        continue
    parsed = [parse_slice_hdr(raw, sps_info) for (_, raw) in hdrs]
    parsed = [h for h in parsed if h is not None]
    if not parsed:
        continue
    max_vcl_any = max(max_vcl_any, len(parsed))
    if len(parsed) > 1:
        groups_with_multi += 1
    parts = []
    for h in parsed:
        if h['first']:
            parts.append("first=1 addr=0")
        else:
            a = h['addr']
            if a is None:
                parts.append("first=0 addr=?")
            else:
                all_addrs.add(a)
                if sps_info:
                    px = a * sps_info['ctb_size']
                    parts.append(f"first=0 addr={a} ({px}px)")
                else:
                    parts.append(f"first=0 addr={a}")
    print(f"  ts={ts}  {len(parsed)} VCL:  {' | '.join(parts)}")

# Scan all groups for the global maximum VCL count (not just the sample).
all_max_vcl = max(
    (len(vcl_hdrs.get((video_ssrc, ts), [])) for ts in v_ts),
    default=0
)

print(f"\n  Max VCL NALs in any single group (all {len(v_ts)} groups): {all_max_vcl}")

if all_addrs:
    sorted_addrs = sorted(all_addrs)
    print(f"  Non-zero slice_segment_address values: {sorted_addrs[:24]}"
          + (" ..." if len(sorted_addrs) > 24 else ""))
    if sps_info and len(sorted_addrs) >= 2:
        step = sorted_addrs[1] - sorted_addrs[0]
        slice_h_px = step * sps_info['ctb_size']
        print(f"  Address step: {step} CTB = {slice_h_px} px/slice  "
              f"(expected for {sps_info['height']}px frame: "
              f"{sps_info['ctbs_h']} CTB rows)")
else:
    print("  Non-zero slice_segment_address values: none observed")
    print("  → every VCL NAL has first_slice_segment_in_pic_flag=1")
    print("  → encoder is producing 1 slice per picture (no spatial split)")

# ── Conclusion ───────────────────────────────────────────────────────────────

print(f"\n── Conclusion ───────────────────────────────────────────────────")

if not v_vcl:
    print("  No video frames captured.")
    sys.exit(1)

marker_ok = (bad == 0)

# Bitstream evidence is the strongest: non-zero slice_segment_address or
# multiple VCL NALs in one group both prove real multi-slice encoding.
bitstream_slices = (all_max_vcl > 1) or bool(all_addrs)

if (vcl_per_frame >= 1.5 or bitstream_slices) and marker_ok:
    if per_slice_au:
        mode_str = f"per-slice AU, ~{len(v_ts)/n_frames_est:.0f} AUs/frame"
    else:
        mode_str = "standard"
    print(f"  SLICES CONFIRMED  ({mode_str})")
    print(f"  {vcl_per_frame:.1f} VCL NALs/frame")
    if all_addrs:
        print(f"  Bitstream: slice_segment_address = {sorted(all_addrs)[:8]}")
    if len(ssrcs) > 1:
        other = sum(ssrc_pkt_count[s] for s in ssrcs if s != video_ssrc)
        print(f"  {other} packets on other SSRC(s) excluded (likely audio)")
    sys.exit(0)
elif not marker_ok:
    print(f"  WARNING: {bad} group(s) have marker not on last packet — "
          f"packetiser error")
    sys.exit(1)
else:
    print(f"  NO SLICES  —  {vcl_per_frame:.1f} VCL/frame, "
          f"max {all_max_vcl} VCL/group, no non-zero slice addresses")
    print(f"  MI_VENC_SetH265SliceSplit is not producing multiple VCL NALs")
    sys.exit(1)
