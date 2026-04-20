# WFB-NG TX Integration

Embeds wfb-ng packet injection directly inside the `venc` process. RTP packets are
passed to `wfb_tx_send()` inline — no unix socket, no separate `wfb_tx` process.

## Configuration

Set `outgoing.server` to a `wfb://` URI. Everything after `wfb://` is the argument
string passed verbatim to `wfb_tx_init()`, using the same flags as the standalone
`wfb_tx` CLI. `stream_mode` must be `"rtp"`.

```json
"outgoing": {
    "enabled": true,
    "server": "wfb:// -K /etc/drone.key -M 4 -B 20 -k 8 -n 10 -S 0 -L 1 -i 7669206 wlan0",
    "stream_mode": "rtp"
}
```

The last positional argument must be the wlan interface name (e.g. `wlan0`).

Socket-transport flags from the standalone CLI (`-U`, `-C`, `-T`, `-d`) are silently
ignored — they have no meaning in embedded mode.

Live server switching (`/api/v1/set?outgoing.server=...`) is not supported for
`wfb://`. Restart venc to change WFB parameters.

### Inject retries (ENOBUFS)

When the kernel wireless TX queue is full, `sendmsg()` returns `ENOBUFS`. Without
retries the packet is silently dropped. The embedded module retries up to `-J` times
with a `-E` µs sleep between each attempt.

The default (`-J 10 -E 5000`) retries 10 times with 5 ms spacing — up to 50 ms of
total wait, only on congestion. Reduce `-J` if latency is more important than
reliability; increase it on noisy channels. Set `-J 0` to disable retries entirely.

### Accepted flags

| Flag | Default | Description |
|------|---------|-------------|
| `-K <path>` | `/etc/drone.key` | Keypair file |
| `-k <n>` | 8 | FEC data shards |
| `-n <n>` | 12 | FEC total shards (parity = n − k) |
| `-p <n>` | 0 | Radio port (channel sub-id) |
| `-F <ms>` | 0 | FEC flush delay |
| `-B <MHz>` | 20 | Channel bandwidth (20 / 40 / 80) |
| `-G s\|l` | long | Guard interval (short / long) |
| `-S <n>` | 0 | STBC streams |
| `-L <0\|1>` | 0 | LDPC |
| `-M <n>` | 1 | MCS index |
| `-N <n>` | 1 | VHT spatial streams |
| `-V` | off | Force VHT mode |
| `-i <id>` | 0 | Link ID (24-bit) |
| `-e <epoch>` | 0 | Session epoch |
| `-m` | off | Mirror mode (inject on all interfaces) |
| `-f data\|rts` | data | Frame type |
| `-Q` | off | Use qdisc bypass |
| `-P <mark>` | 0 | fwmark |
| `-J <n>` | 10 | Inject retries on ENOBUFS |
| `-E <µs>` | 5000 | Inject retry delay |

## Architecture

```
venc encoder output (RTP packetizer)
        │
        ▼  star6e_output_send_rtp_parts()
        │  maruko_rtp_write()             ← per-packet, inline
        │
        ▼
  wfb_tx_send(buf, size)
        │
        ▼
  RawSocketTransmitter  (C++11, src/wfbtx/)
  ├── FEC encode  — zfex Reed-Solomon k/n
  ├── encrypt     — libsodium chacha20-poly1305
  └── inject      — PF_PACKET sendmsg() → wlan0
```

The C++ wfbtx objects are pre-compiled with `$(CXX)` and linked into the C99 `venc`
binary via `$(CC)` + `-lstdc++`. The public interface is `extern "C"` throughout.

Both backends (star6e and maruko) are supported. Compact stream mode is not
supported with `wfb://` — the server URI parser rejects it at startup.

## Key File

Default path: `/etc/drone.key`

Override with `-K /path/to/key` in the `wfb://` args string.

Generate a keypair on the device (must match the receiver's public key):

```sh
wfb_keygen /etc/drone.key
```

## Internal C API (`src/wfbtx/`)

These functions are called by the output pipeline and are not exposed to config.

```c
#include "wfb_tx.h"

/* Parse args string and initialise the transmitter. */
int  wfb_tx_init_from_str(const char *args_str);

/* Low-level init from pre-split argc/argv (same flags as wfb_tx CLI). */
int  wfb_tx_init(int argc, char * const *argv);

/* Update radiotap header at runtime (MCS/bandwidth/STBC/LDPC/GI/VHT). */
int  wfb_tx_setup_radio(uint8_t stbc, int ldpc, int short_gi,
                         uint8_t bandwidth, uint8_t mcs_index,
                         int vht_mode, uint8_t vht_nss);

/* Change FEC k/n at runtime — flushes open block, resends session keys. */
int  wfb_tx_setup_fec(int k, int n);

/* Encrypt, FEC-encode, and inject one RTP packet. Handles session key timer. */
void wfb_tx_send(const uint8_t *buf, size_t size);

/* Tear down transmitter and release all resources. */
void wfb_tx_destroy(void);
```

`wfb_tx_init_from_str()` tokenises the args string on whitespace, prepends a
synthetic `argv[0]`, and calls `wfb_tx_init()`. Max 31 tokens.

`wfb_tx_send()` announces the session key once per second
(`SESSION_KEY_ANNOUNCE_MSEC = 1000`). It is a no-op if `init` was not called.

`wfb_tx_setup_radio()` and `wfb_tx_setup_fec()` are available for runtime
reconfiguration (e.g. via a future API endpoint) without restarting venc.

## Source Files (`src/wfbtx/`)

| File | Origin | Purpose |
|------|--------|---------|
| `wfb_tx.h` | new | C API header (`extern "C"`) |
| `wfb_tx.cpp` | new | API implementation; global `RawSocketTransmitter` singleton |
| `tx_core.cpp` | extracted from wfb-ng `tx.cpp` | `Transmitter`, `RawSocketTransmitter`, `init_radiotap_header()` — loop and `main()` stripped |
| `wifibroadcast.cpp` | wfb-ng verbatim | `get_time_ms()`, `string_format()`, utility helpers |
| `wifibroadcast.hpp` | wfb-ng verbatim | Protocol structs, constants, `SESSION_KEY_ANNOUNCE_MSEC` |
| `tx.hpp` | wfb-ng verbatim | Class declarations |
| `tx_cmd.h` | wfb-ng verbatim | `cmd_req_t`, `CMD_SET_FEC`, `CMD_SET_RADIO` |
| `zfex.c/.h` + helpers | wfb-ng verbatim | Reed-Solomon FEC (SIMD-optimised) |
| `ieee80211_radiotap.h` | wfb-ng verbatim | Radiotap iterator |
| `pcap.h` | stub | Satisfies `#include <pcap.h>` in `wifibroadcast.hpp`; TX never uses pcap |

`tx_core.cpp` contains lines 1–740 and 1095–1211 of upstream `tx.cpp` (all class
implementations and `init_radiotap_header()`). `data_source()`, `packet_injector()`,
and `main()` are excluded.

## Build

### Prerequisites

Run once before the first `make build` to cross-compile libsodium:

```sh
./wfb/build_wfb_tx.sh
```

This produces `wfb/build/sodium-install/lib/libsodium.a` used by the venc link step.
The same script also builds the standalone `wfb_tx` binary with SHM input support
(a separate deployment mode, not required for the embedded integration).

### Makefile integration

The wfbtx module is built and linked automatically as part of `make build`.

```
# C++ sources — gnu++11
src/wfbtx/wifibroadcast.cpp  →  out/<soc>/wfbtx/wifibroadcast.o
src/wfbtx/tx_core.cpp        →  out/<soc>/wfbtx/tx_core.o
src/wfbtx/wfb_tx.cpp         →  out/<soc>/wfbtx/wfb_tx.o

# C source — gnu99 + SIMD FEC defines
src/wfbtx/zfex.c             →  out/<soc>/wfbtx/zfex.o

# Linked into venc: $(CC) ... $(WFB_TX_OBJS) ... -lstdc++ libsodium.a
```

NEON SIMD is enabled on star6e via `-mfpu=neon-vfpv4 -DZFEX_UNROLL_ADDMUL_SIMD=8
-DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD`. Maruko builds without NEON
(no `-mfpu` flag in `SOC_CFLAGS`); zfex falls back to the portable C path.

### Supported targets

| `SOC_BUILD` | Toolchain | ABI |
|-------------|-----------|-----|
| `star6e` (default) | `arm-openipc-linux-gnueabihf-g++` | armv7-hf |
| `maruko` | `arm-openipc-linux-musleabihf-g++` | armv7-hf (musl) |
