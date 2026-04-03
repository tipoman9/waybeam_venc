# Unix Socket Output

## Purpose

This document describes the `unix://` outgoing transport for video output.
It is intended for integrations that want to feed encoded video packets directly
into a local Unix datagram consumer such as WFB-ng `tx.cpp` without using UDP.

## Scope

- Applies to `outgoing.server`
- Supports Star6E and Maruko video output
- Supports both `rtp` and `compact` stream modes
- Uses Linux abstract Unix domain datagram sockets

## URI Format

Use this form in `/etc/venc.json`:

```json
{
  "outgoing": {
    "enabled": true,
    "server": "unix://wfb-video",
    "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "connectedUdp": false
  }
}
```

The name after `unix://` is the abstract socket name. On Linux this maps to
the socket address `@wfb-video`.

This is not a filesystem pathname socket. No socket file is created under
`/tmp`, `/run`, or any other directory.

## External WFB-ng Compatibility

WFB-ng `tx.cpp` binds abstract Unix datagram sockets when started with `-U`.
The expected sender/receiver pairing is:

```bash
# WFB-ng side
wfb_tx -U rtp_local ...

# venc config
"server": "unix://rtp_local"
```

The names must match exactly.

WFB-ng reads packets with `recvmsg()` from an abstract `AF_UNIX` / `SOCK_DGRAM`
socket, so each `sendmsg()` or `sendto()` call from venc becomes one received
datagram on the WFB side.

## Transport Behavior

### RTP mode

- Each RTP packet is sent as one Unix datagram.
- HEVC packetization behavior is unchanged from UDP mode.
- This is the recommended mode when the downstream consumer expects RTP payloads.

### Compact mode

- Compact-mode packets are sent as Unix datagrams instead of UDP datagrams.
- Packet chunking behavior remains the same as UDP compact mode.

## Configuration Rules

- `udp://HOST:PORT` continues to use UDP.
- `unix://NAME` uses an abstract Unix datagram socket.
- `shm://NAME` continues to use the RTP shared-memory ring.
- `connectedUdp` only affects `udp://` destinations and is ignored for `unix://`.
- `shm://` still requires `streamMode: "rtp"`.

## Live API Changes

`outgoing.server` live changes via the HTTP API remain limited to the existing
UDP flow.

`unix://` output is supported when configured before pipeline startup or when
applied through a restart/reinit path that rebuilds the output transport.

Examples:

```bash
wget -q -O- 'http://127.0.0.1/api/v1/set?outgoing.server=udp://192.168.2.2:5600'
```

Live switching to `unix://` or `shm://` is not supported. Switching to or from
those transports requires pipeline restart or config reload.

## Limitations

- This feature is for video output transport.
- Star6E audio output still requires `udp://` video transport. If audio is
  enabled while `unix://` video output is configured, audio init fails.
- RTP sidecar behavior is unchanged and still uses its existing UDP sidecar
  mechanism.
- The Unix transport uses abstract sockets and is therefore Linux-specific.

## Operational Notes

- Use short, stable socket names.
- The abstract namespace is global to the host network namespace, so name
  collisions are possible if multiple processes bind the same socket name.
- If the external consumer is not bound yet, `sendmsg()` may fail and packets
  may be dropped, just as UDP output can drop packets when no receiver is ready.

## Recommended WFB-ng Setup

For local WFB-ng injection, use matching names and keep packet size aligned:

```json
{
  "outgoing": {
    "enabled": true,
    "server": "unix://rtp_local",
    "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "connectedUdp": false,
    "audioPort": 5601,
    "sidecarPort": 5602
  }
}
```

```bash
wfb_tx -U rtp_local ...
```

If the downstream transport expects raw encoded chunks instead of RTP, switch
`streamMode` to `compact`.