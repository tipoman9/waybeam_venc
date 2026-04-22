#!/usr/bin/env python3
"""
rtp_slice_check.py — RTP timestamp analyzer for confirming H265 slice split

Captures raw UDP/RTP packets on a given port and groups them by SSRC+timestamp.
With H265 slice split active, all slices of a single frame share one RTP
timestamp; only the last RTP packet of the last slice has the marker bit set
(RFC 7798). Without slicing, each frame produces one timestamp group whose
size is determined only by MTU fragmentation (FU-A).

Stream separation is done by SSRC (bytes 8-11 of the RTP header), so audio and
video on the same port are cleanly separated without heuristics. The video SSRC
is identified as the one with the highest average packets-per-group. Packets
within each group are sorted by RTP sequence number before analysis so that
UDP reordering does not produce false marker-bit violations.

Usage:
    python3 rtp_slice_check.py [PORT [N_PACKETS]]

    PORT        UDP port to listen on (default: 5600)
    N_PACKETS   Number of RTP packets to capture before reporting (default: 500)

The encoder under test must be configured to send direct UDP RTP (not WFB).
Set outgoing.server to "udp://<this-host>:<PORT>" in venc.json and restart.

Exit code 0 = slices confirmed, 1 = no slices detected or no packets received.
"""
import socket, struct, sys
from collections import OrderedDict, Counter, defaultdict

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5600
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 500

print(f"Listening on 0.0.0.0:{PORT}, capturing {N} RTP packets...", flush=True)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(10.0)

# (ssrc, ts) -> [(seq, marker), ...]
groups = defaultdict(list)
ssrc_pkt_count = Counter()
count = 0

try:
    while count < N:
        data, _ = sock.recvfrom(2048)
        if len(data) < 12:
            continue
        marker = bool(data[1] & 0x80)
        seq    = struct.unpack_from(">H", data, 2)[0]
        ts     = struct.unpack_from(">I", data, 4)[0]
        ssrc   = struct.unpack_from(">I", data, 8)[0]
        groups[(ssrc, ts)].append((seq, marker))
        ssrc_pkt_count[ssrc] += 1
        count += 1
        if count % 50 == 0:
            print(f"  {count}/{N} packets received...", flush=True)
except socket.timeout:
    print(f"Timeout after {count} packets")

sock.close()

if not groups:
    print("\nNo packets received.")
    sys.exit(1)

# Sort each group by sequence number (wrapping-safe for normal streams)
for key in groups:
    groups[key].sort(key=lambda p: p[0])

# ── Per-SSRC breakdown ───────────────────────────────────────────────────────
ssrcs = list(ssrc_pkt_count.keys())

print(f"\n--- Results: {count} packets, {len(groups)} timestamp groups, "
      f"{len(ssrcs)} SSRC(s) ---")

ssrc_groups = {}   # ssrc -> { ts: [(seq,marker), ...] }
for (ssrc, ts), pkts in groups.items():
    ssrc_groups.setdefault(ssrc, OrderedDict())[ts] = pkts

# Identify video SSRC: highest average packets-per-group
def avg_group_size(g):
    sizes = [len(v) for v in g.values()]
    return sum(sizes) / len(sizes) if sizes else 0

video_ssrc = max(ssrc_groups, key=lambda s: avg_group_size(ssrc_groups[s]))

for ssrc, g in ssrc_groups.items():
    label = "VIDEO" if ssrc == video_ssrc else "other"
    sizes = [len(v) for v in g.values()]
    avg   = sum(sizes) / len(sizes) if sizes else 0
    ts_vals = sorted(g.keys())
    if len(ts_vals) >= 2:
        deltas = [ts_vals[i+1] - ts_vals[i] for i in range(len(ts_vals)-1)]
        common_delta = Counter(deltas).most_common(1)[0][0]
        fps_str = f"{90000/common_delta:.1f} fps" if common_delta else "?"
    else:
        fps_str = "?"
    print(f"  SSRC 0x{ssrc:08X}  [{label}]  "
          f"{ssrc_pkt_count[ssrc]:4d} pkts  {len(g):3d} groups  "
          f"avg {avg:.1f} pkts/group  ~{fps_str}")

# ── Video stream analysis ────────────────────────────────────────────────────
vg = ssrc_groups[video_ssrc]
v_ts = sorted(vg.keys())
v_sizes = [len(vg[ts]) for ts in v_ts]

print(f"\n── Video groups (SSRC 0x{video_ssrc:08X}) — first 20 ────────────────────")
print(f"  {'Timestamp':>12}  {'Pkts':>5}  Marker at")
for i, ts in enumerate(v_ts):
    if i >= 20:
        print(f"  ... ({len(v_ts)-20} more frames)")
        break
    pkts   = vg[ts]
    marked = [j for j, (_, m) in enumerate(pkts) if m]
    mstr   = str(marked) if marked else "—"
    print(f"  {ts:>12}  {len(pkts):>5}  {mstr}")

# ── Packets-per-frame distribution ──────────────────────────────────────────
print(f"\n── Packets per frame (video) ────────────────────────────────────")
dist = Counter(v_sizes)
max_count = max(dist.values())
for size in sorted(dist):
    bar = "█" * int(round(dist[size] / max_count * 20))
    print(f"  {size:3d} pkt/frame  x{dist[size]:4d}  {bar}")

v_avg = sum(v_sizes) / len(v_sizes) if v_sizes else 0
print(f"\n  Min {min(v_sizes)}  /  Avg {v_avg:.1f}  /  Max {max(v_sizes)}  "
      f"({len(v_sizes)} frames)")

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
if bad:
    print(f"  Marker NOT on last packet  : {bad}  ← PROBLEM")
else:
    print(f"  Marker misplaced           : 0  ✓")

# ── Conclusion ───────────────────────────────────────────────────────────────
print(f"\n── Conclusion ───────────────────────────────────────────────────")
if not v_sizes:
    print("  No video frames captured.")
    sys.exit(1)

marker_ok = (bad == 0)

if v_avg >= 2 and marker_ok:
    print(f"  SLICES CONFIRMED")
    print(f"  {v_avg:.1f} RTP packets/frame (video average)")
    print(f"  Marker bit correctly on last packet of every frame ✓")
    if len(ssrcs) > 1:
        other_pkts = sum(ssrc_pkt_count[s] for s in ssrcs if s != video_ssrc)
        print(f"  {other_pkts} packets on other SSRC(s) excluded from analysis "
              f"(likely audio)")
    sys.exit(0)
elif not marker_ok:
    print(f"  WARNING: {bad} frame(s) have marker bit not on last packet — "
          f"packetizer error")
    sys.exit(1)
else:
    print(f"  NO SLICES DETECTED — avg {v_avg:.1f} pkt/frame "
          f"(fragmentation only, not slice split)")
    sys.exit(1)
