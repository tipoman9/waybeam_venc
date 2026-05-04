#!/usr/bin/env python3
"""
hevc_slice_reassembler.py — per-slice AU reassembler for waybeam_venc perSliceAu mode.

Receives the per-slice H.265 RTP stream produced when perSliceAu=true and
reassembles slices back into complete H.265 frame access units.  A lost RTP
packet results in only the affected 64-pixel row-band being concealed by the
downstream decoder; all other rows decode normally.

This is necessary because standard RTP pipelines (rtpjitterbuffer →
rtph265depay) flush their entire frame buffer on a gap event, turning a
single lost packet into a lost frame.  By doing our own assembly we keep
whatever slices did arrive and let the H.265 decoder conceal only the gap.

Usage:
    python3 hevc_slice_reassembler.py [OPTIONS]

    -p, --port PORT         UDP port to listen on (default: 5600)
    --fps FPS               Encoder frame rate (default: 60)
    --height H              Encoded frame height in pixels (default: 1080)
    --slice-rows N          sliceRows encoder setting (default: 4)
    --decoder NAME          GStreamer decoder element: vah265dec (default) or avdec_h265
    --addr ADDR             Bind address (default: 0.0.0.0; use 127.0.0.1 for loopback)

Requirements:
    python3-gi
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
    gstreamer1.0-vaapi  (or gstreamer1.0-libav for avdec_h265)
"""

import gi
gi.require_version('Gst', '1.0')
gi.require_version('GLib', '2.0')
from gi.repository import Gst, GLib

import argparse
import socket
import struct
import threading
import time
import sys

# ── CLI ──────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-p', '--port', type=int, default=5600)
    p.add_argument('--fps',        type=int, default=60)
    p.add_argument('--height',     type=int, default=1080)
    p.add_argument('--slice-rows', type=int, default=4, dest='slice_rows')
    p.add_argument('--decoder',    default='vah265dec',
                   choices=['vah265dec', 'avdec_h265'])
    p.add_argument('--addr',       default='0.0.0.0')
    return p.parse_args()

# ── RTP parsing ──────────────────────────────────────────────────────────────

def rtp_parse(data):
    """Return (seq, ts, ssrc, payload) or None on malformed packet."""
    if len(data) < 12 or (data[0] >> 6) != 2:
        return None
    cc      = data[0] & 0x0F
    has_ext = (data[0] >> 4) & 1
    seq, ts, ssrc = struct.unpack_from('!HII', data, 2)
    off = 12 + cc * 4
    if has_ext:
        if len(data) < off + 4:
            return None
        ext_words = struct.unpack_from('!H', data, off + 2)[0]
        off += 4 + ext_words * 4
    if off > len(data):
        return None
    return seq, ts, ssrc, data[off:]

# ── HEVC NAL helpers ─────────────────────────────────────────────────────────
#
# HEVC NAL unit header (2 bytes, ITU-T H.265 §7.3.1.2):
#   byte 0: F(1) | nal_unit_type(6) | nuh_layer_id[5](1)
#   byte 1: nuh_layer_id[4:0](5) | nuh_temporal_id_plus1(3)

HEVC_VPS = 32
HEVC_SPS = 33
HEVC_PPS = 34
HEVC_FU  = 49   # Fragmentation Unit (RFC 7798 §4.4.3)
HEVC_AP  = 48   # Aggregation Packet (RFC 7798 §4.4.2) — disabled by disablePacketAggregation

START_CODE = b'\x00\x00\x00\x01'

def hevc_nal_type(nal):
    return (nal[0] >> 1) & 0x3F if len(nal) >= 2 else -1

def hevc_is_vcl(t):
    return 0 <= t <= 31

# ── FU (Fragmentation Unit) reassembly ──────────────────────────────────────
#
# FU packet layout after the 2-byte outer HEVC NAL header (type=49):
#   byte 0: S(1) | E(1) | FuType(6)
#   bytes 1+: NAL payload fragment
#
# To reconstruct the original NAL header:
#   byte 0 = (FuType << 1) | (outer_hdr[0] & 0x01)   — preserves layer_id MSB
#   byte 1 = outer_hdr[1]                              — preserves layer_id LSBs + TID

class FuBuf:
    __slots__ = ('parts', 'hdr', 'done')

    def __init__(self):
        self.parts = []
        self.hdr   = b''
        self.done  = False

    def feed(self, outer_hdr, payload):
        if len(payload) < 1:
            return
        fh       = payload[0]
        fu_type  = fh & 0x3F
        if fh & 0x80:   # start bit
            self.parts = [payload[1:]]
            self.hdr   = bytes([
                ((fu_type << 1) & 0x7E) | (outer_hdr[0] & 0x01),
                outer_hdr[1],
            ])
            self.done  = False
        else:
            self.parts.append(payload[1:])
        if fh & 0x40:   # end bit
            self.done = True

    def assemble(self):
        """Return complete NAL bytes or None if FU is incomplete."""
        if not self.done or not self.hdr:
            return None
        return self.hdr + b''.join(self.parts)

# ── Per-frame accumulator ────────────────────────────────────────────────────

class Frame:
    __slots__ = ('slices', 'vps', 'sps', 'pps', 'last_rx')

    def __init__(self):
        self.slices  = {}           # slice_index → NAL bytes
        self.vps     = None
        self.sps     = None
        self.pps     = None
        self.last_rx = time.monotonic()

    def add(self, slice_idx, nal):
        t = hevc_nal_type(nal)
        if   t == HEVC_VPS:         self.vps = nal
        elif t == HEVC_SPS:         self.sps = nal
        elif t == HEVC_PPS:         self.pps = nal
        elif hevc_is_vcl(t):
            self.slices[slice_idx] = nal
            self.last_rx = time.monotonic()

    def assemble(self, num_slices, fallback_params):
        """
        Build an Annex-B bytestream from accumulated NALs.

        Returns (bytestream, missing_slice_indices).
        Returns (None, []) if the first slice is absent — that AU would be
        an invalid H.265 bitstream and the decoder would reject it; the
        caller should skip this frame and let vah265dec freeze the display.
        """
        if 0 not in self.slices:
            return None, []

        vps = self.vps or (fallback_params[0] if fallback_params else None)
        sps = self.sps or (fallback_params[1] if fallback_params else None)
        pps = self.pps or (fallback_params[2] if fallback_params else None)

        out = bytearray()
        if vps: out += START_CODE + vps
        if sps: out += START_CODE + sps
        if pps: out += START_CODE + pps

        missing = []
        for i in range(num_slices):
            if i in self.slices:
                out += START_CODE + self.slices[i]
            else:
                missing.append(i)

        return bytes(out), missing

# ── Reassembler ──────────────────────────────────────────────────────────────

class Reassembler:
    def __init__(self, appsrc, fps, frame_ticks, slice_ticks, num_slices, slice_h):
        self.appsrc       = appsrc
        self.fps          = fps
        self.frame_ticks  = frame_ticks
        self.slice_ticks  = slice_ticks
        self.num_slices   = num_slices
        self.slice_h      = slice_h

        # WFB-NG FEC delivers packets for a single frame spread over up to
        # ~300 ms of wall-clock time.  Flush only via the timeout path so that
        # no slice is cut off by a packet from a later frame arriving first.
        self.flush_timeout = 0.4

        self.frames       = {}      # frame_index → Frame
        self.fus          = {}      # (ssrc, ts) → FuBuf
        self.anchor       = None    # RTP timestamp of the first frame's start
        self.last_params  = None    # (vps, sps, pps) from last complete frame
        self.last_flushed = -1

        self.pts_ns       = 0
        self.frame_ns     = int(1e9 / fps)

        self.lock         = threading.Lock()
        self.stats        = {'frames': 0, 'lost_first': 0, 'partial': 0}

    # ── timestamp arithmetic ─────────────────────────────────────────────────

    def _fidx(self, ts):
        if self.anchor is None:
            # Use the first packet's timestamp as anchor so that it maps to
            # fidx=0 / sidx=0 regardless of the SDK's random initial ts.
            # Snapping to a frame_ticks multiple would work only if T_init
            # happened to be frame-aligned, which it never is in practice.
            self.anchor = ts
        return int(((ts - self.anchor) & 0xFFFFFFFF) // self.frame_ticks)

    def _sidx(self, ts, fidx):
        base = (self.anchor + fidx * self.frame_ticks) & 0xFFFFFFFF
        diff = (ts - base) & 0xFFFFFFFF
        return int(round(diff / self.slice_ticks)) if self.slice_ticks else 0

    # ── public API ───────────────────────────────────────────────────────────

    def rx(self, data):
        """Process one UDP datagram."""
        p = rtp_parse(data)
        if not p:
            return
        seq, ts, ssrc, payload = p
        if not payload:
            return

        nt = hevc_nal_type(payload)

        with self.lock:
            fidx = self._fidx(ts)
            sidx = self._sidx(ts, fidx)

            frame = self.frames.setdefault(fidx, Frame())

            if nt == HEVC_FU:
                key = (ssrc, ts)
                fu  = self.fus.setdefault(key, FuBuf())
                fu.feed(payload[:2], payload[2:])
                if fu.done:
                    nal = fu.assemble()
                    if nal:
                        frame.add(sidx, nal)
                    del self.fus[key]
            elif nt == HEVC_AP:
                # Should not appear with disablePacketAggregation=true, but
                # handle it defensively: walk the AP NALU list.
                off = 2          # skip 2-byte AP NAL header
                while off + 2 <= len(payload):
                    nlen = struct.unpack_from('!H', payload, off)[0]
                    off += 2
                    if off + nlen > len(payload):
                        break
                    frame.add(sidx, payload[off:off + nlen])
                    off += nlen
            else:
                frame.add(sidx, payload)

    def tick(self):
        """GLib timer callback: flush frames that have timed out."""
        with self.lock:
            now = time.monotonic()
            for fidx in sorted(self.frames):
                if now - self.frames[fidx].last_rx > self.flush_timeout:
                    self._flush(fidx)
            for fidx in [f for f in self.frames if f <= self.last_flushed]:
                del self.frames[fidx]
        return True     # keep the GLib timer alive

    # ── internal ─────────────────────────────────────────────────────────────

    def _flush(self, fidx):
        if fidx <= self.last_flushed:
            return
        self.last_flushed = fidx

        frame = self.frames.get(fidx)
        if not frame or not frame.slices:
            return

        data, missing = frame.assemble(self.num_slices, self.last_params)

        if data is None:
            # First slice was lost — cannot form a valid AU.
            self.stats['lost_first'] += 1
            print(f"[frame {fidx}] first slice lost — skipping "
                  f"(decoder will freeze this frame)")
            self.pts_ns += self.frame_ns
            return

        self.stats['frames'] += 1
        if missing:
            self.stats['partial'] += 1
            bands = [f"{i * self.slice_h}–{(i + 1) * self.slice_h - 1}px"
                     for i in missing]
            print(f"[frame {fidx}] {len(missing)}/{self.num_slices} slices missing: "
                  f"rows {bands}")

        if frame.vps and frame.sps and frame.pps:
            self.last_params = (frame.vps, frame.sps, frame.pps)

        buf          = Gst.Buffer.new_wrapped(data)
        buf.pts      = self.pts_ns
        buf.duration = self.frame_ns
        self.pts_ns += self.frame_ns

        ret = self.appsrc.emit('push-buffer', buf)
        if ret != Gst.FlowReturn.OK:
            print(f"[frame {fidx}] appsrc push-buffer returned {ret}", file=sys.stderr)

    def print_stats(self):
        s = self.stats
        print(f"\n── Stats ────────────────────────────────────────\n"
              f"  Frames emitted : {s['frames']}\n"
              f"  Partial frames : {s['partial']}\n"
              f"  Skipped (no first slice): {s['lost_first']}")

# ── GStreamer pipeline ────────────────────────────────────────────────────────

def build_pipeline(decoder):
    pipe_desc = (
        'appsrc name=src format=time is-live=true block=false '
        '   caps="video/x-h265,stream-format=byte-stream,alignment=au" '
        '! h265parse '
        f'! {decoder} '
        '! autovideosink sync=false'
    )
    return Gst.parse_launch(pipe_desc)

# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    RTP_CLOCK   = 90000
    FRAME_TICKS = RTP_CLOCK // args.fps
    MB_ROWS     = (args.height + 15) // 16
    NUM_SLICES  = (MB_ROWS + args.slice_rows - 1) // args.slice_rows
    SLICE_TICKS = FRAME_TICKS // NUM_SLICES if NUM_SLICES > 1 else FRAME_TICKS
    SLICE_H     = args.slice_rows * 16   # pixel rows per slice band

    print(f"Config: fps={args.fps}  height={args.height}  "
          f"slice_rows={args.slice_rows}  decoder={args.decoder}")
    print(f"  frame_ticks={FRAME_TICKS}  slice_ticks={SLICE_TICKS}  "
          f"num_slices={NUM_SLICES}")
    print(f"  Each slice = {SLICE_H}px tall, "
          f"RTP ts step = {SLICE_TICKS} ticks")

    Gst.init(None)
    pipeline = build_pipeline(args.decoder)
    appsrc   = pipeline.get_by_name('src')

    r = Reassembler(appsrc, args.fps, FRAME_TICKS, SLICE_TICKS, NUM_SLICES, SLICE_H)

    # Flush timed-out frames every 20 ms from the GLib main loop.
    GLib.timeout_add(20, r.tick)

    # Bus message handler: print errors and EOS.
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    loop = GLib.MainLoop()

    def on_message(bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            print(f"GStreamer error: {err.message}\n  {dbg}", file=sys.stderr)
            loop.quit()
        elif msg.type == Gst.MessageType.EOS:
            loop.quit()

    bus.connect('message', on_message)

    pipeline.set_state(Gst.State.PLAYING)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.addr, args.port))
    sock.settimeout(0.5)

    print(f"Listening on {args.addr}:{args.port} …  Ctrl-C to stop.\n")

    def rx_worker():
        while True:
            try:
                r.rx(sock.recv(65536))
            except socket.timeout:
                continue
            except OSError:
                break

    rx_thread = threading.Thread(target=rx_worker, daemon=True)
    rx_thread.start()

    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        r.print_stats()
        pipeline.set_state(Gst.State.NULL)
        sock.close()


if __name__ == '__main__':
    main()
