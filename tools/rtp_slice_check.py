#!/usr/bin/env python3
"""
rtp_slice_check.py — RTP timestamp analyzer for confirming H265 slice split

Captures raw UDP/RTP packets on a given port and groups them by RTP timestamp.
With H265 slice split active, all slices of a single frame share one RTP
timestamp; only the last RTP packet of the last slice has the marker bit set
(RFC 7798). Without slicing, each frame typically produces a single timestamp
group whose size depends only on fragmentation (FU-A) at the MTU boundary.

Usage:
    python3 rtp_slice_check.py [PORT [N_PACKETS]]

    PORT        UDP port to listen on (default: 5610)
    N_PACKETS   Number of RTP packets to capture before reporting (default: 400)

The encoder under test must be configured to send direct UDP RTP (not WFB).
Set outgoing.server to "udp://<this-host>:<PORT>" in venc.json and restart.

Expected output with sliceRows=4 on 1920x1080:
  ~17 video frames visible per 400 packets (68 MB-rows / 4 = 17 slices/frame)
  Each video timestamp group has 4–55 packets depending on slice/MTU sizing
  Marker bit appears only at the last index of each video timestamp group
  Single-packet groups with a different timestamp cadence are audio frames

Exit code 0 = slices confirmed, 1 = no slices detected or no packets received.
"""
import socket, struct, sys, collections

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5610
N = int(sys.argv[2]) if len(sys.argv) > 2 else 400

print(f"Listening on 0.0.0.0:{PORT}, capturing {N} RTP packets...", flush=True)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(10.0)

# ts -> [marker, marker, ...]  (True = marker bit set)
ts_groups = collections.OrderedDict()
count = 0

try:
    while count < N:
        data, addr = sock.recvfrom(2048)
        if len(data) < 12:
            continue
        b0, b1 = data[0], data[1]
        marker = bool(b1 & 0x80)
        ts = struct.unpack_from(">I", data, 4)[0]
        if ts not in ts_groups:
            ts_groups[ts] = []
        ts_groups[ts].append(marker)
        count += 1
        if count % 50 == 0:
            print(f"  {count}/{N} packets received...", flush=True)
except socket.timeout:
    print(f"Timeout after {count} packets")

sock.close()

print(f"\n--- Results ({count} packets, {len(ts_groups)} unique timestamps) ---")

group_sizes = [len(v) for v in ts_groups.values()]
if not group_sizes:
    print("No packets received.")
    sys.exit(1)

print(f"{'Timestamp':>12}  {'Pkts':>6}  {'Marker positions'}")
for i, (ts, markers) in enumerate(ts_groups.items()):
    if i >= 20:
        print(f"  ... ({len(ts_groups)-20} more groups)")
        break
    marker_pos = [j for j, m in enumerate(markers) if m]
    print(f"  {ts:>12}  {len(markers):>6}  {marker_pos}")

print(f"\nPackets per timestamp group:")
from collections import Counter
dist = Counter(group_sizes)
for size in sorted(dist):
    print(f"  {size:3d} pkt/group  x {dist[size]:4d}")

avg = sum(group_sizes) / len(group_sizes) if group_sizes else 0
print(f"\nAvg packets/group: {avg:.1f}")
print(f"Min: {min(group_sizes)}  Max: {max(group_sizes)}")

if avg >= 8:
    print("\nCONCLUSION: SLICES CONFIRMED — multiple packets share same RTP timestamp")
    sys.exit(0)
elif avg >= 3:
    print("\nCONCLUSION: POSSIBLE SLICES — some grouping observed, check manually")
    sys.exit(0)
else:
    print("\nCONCLUSION: NO SLICES DETECTED — each timestamp has ~1 packet (fragmentation only)")
    sys.exit(1)
