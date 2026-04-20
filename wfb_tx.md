# Embedded wfb-ng TX Module (`src/wfbtx/`)

Integrates wfb-ng packet injection directly into the `venc` process, removing the unix-socket
transport hop between venc and a standalone `wfb_tx` binary.

## Architecture

```
venc encoder output
        │
        ▼
  wfb_tx_send()        ← per RTP packet, called inline from encoder output path
        │
        ▼
  RawSocketTransmitter (C++11, src/wfbtx/)
  ├── FEC encode (zfex Reed-Solomon k/n)
  ├── libsodium encrypt
  └── raw 802.11 inject via PF_PACKET sendmsg()
```

The C++ wfbtx objects are pre-compiled to `.o` files with `$(CXX)` and linked into the
`venc` binary (C99) via `$(CC)` + `-lstdc++`. The public API uses `extern "C"` throughout.

## Source Files

| File | Origin | Purpose |
|------|--------|---------|
| `wfb_tx.h` | new | C API header (`extern "C"`) |
| `wfb_tx.cpp` | new | API implementation, global `RawSocketTransmitter` |
| `tx_core.cpp` | extracted from wfb-ng `tx.cpp` | `Transmitter`, `RawSocketTransmitter`, `init_radiotap_header()` — loop/main stripped |
| `wifibroadcast.cpp` | wfb-ng verbatim | `get_time_ms()`, `string_format()`, socket helpers |
| `wifibroadcast.hpp` | wfb-ng verbatim | Protocol structs, constants, `SESSION_KEY_ANNOUNCE_MSEC` |
| `tx.hpp` | wfb-ng verbatim | Class declarations |
| `tx_cmd.h` | wfb-ng verbatim | `cmd_req_t`, `CMD_SET_FEC`, `CMD_SET_RADIO` |
| `zfex.c/.h` + helpers | wfb-ng verbatim | Reed-Solomon FEC (SIMD-optimised) |
| `ieee80211_radiotap.h` | wfb-ng verbatim | Radiotap iterator |
| `pcap.h` | stub | Satisfies `#include <pcap.h>` in `wifibroadcast.hpp`; TX never uses pcap |

`tx_core.cpp` contains lines 1–740 and 1095–1211 of the upstream `tx.cpp`
(all class implementations + `init_radiotap_header()`). The socket-reading `data_source()`
loop, `packet_injector()`, and `main()` are excluded.

## API

```c
#include "src/wfbtx/wfb_tx.h"

int  wfb_tx_init(int argc, char * const *argv);
int  wfb_tx_setup_radio(uint8_t stbc, int ldpc, int short_gi,
                         uint8_t bandwidth, uint8_t mcs_index,
                         int vht_mode, uint8_t vht_nss);
int  wfb_tx_setup_fec(int k, int n);
void wfb_tx_send(const uint8_t *buf, size_t size);
void wfb_tx_destroy(void);
```

### `wfb_tx_init(argc, argv)`

Parses the same CLI arguments as the standalone `wfb_tx` binary, constructs a
`RawSocketTransmitter`, and sends the initial session key burst.

The last positional argument must be the wlan interface name. Returns 0 on success, -1 on error.

**Accepted flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `-K <path>` | `/etc/drone.key` | Keypair file |
| `-k <n>` | 8 | FEC data shards |
| `-n <n>` | 12 | FEC total shards |
| `-p <n>` | 0 | Radio port (channel sub-id) |
| `-F <ms>` | 0 | FEC flush delay (ms) |
| `-B <MHz>` | 20 | Channel bandwidth (20/40/80) |
| `-G s\|l` | long | Guard interval (short/long) |
| `-S <n>` | 0 | STBC streams |
| `-L <0\|1>` | 0 | LDPC |
| `-M <n>` | 1 | MCS index |
| `-N <n>` | 1 | VHT spatial streams |
| `-V` | off | Force VHT mode |
| `-i <id>` | 0 | Link ID (24-bit) |
| `-e <epoch>` | 0 | Session epoch |
| `-m` | off | Mirror mode (all interfaces) |
| `-f data\|rts` | data | Frame type |
| `-Q` | off | Use qdisc |
| `-P <mark>` | 0 | fwmark |
| `-J <n>` | 0 | Inject retries |
| `-E <µs>` | 5000 | Inject retry delay |

Flags `-U`/`-u`/`-T`/`-C`/`-D`/`-R`/`-s`/`-d`/`-I` (socket/transport) are **not** accepted —
the socket layer is replaced by direct `wfb_tx_send()` calls.

`getopt` state (`optind`) is reset to 1 on entry so venc's own arg parsing is not affected.

**Example init call (equivalent to the old CLI invocation):**

```c
char *wfb_argv[] = {
    "wfb_tx",
    "-K", "/etc/drone.key",
    "-M", "4",
    "-B", "20",
    "-k", "8",
    "-n", "10",
    "-S", "0",
    "-L", "1",
    "-i", "7669206",
    "wlan0",
    NULL
};
int wfb_argc = (sizeof(wfb_argv) / sizeof(wfb_argv[0])) - 1;
if (wfb_tx_init(wfb_argc, wfb_argv) != 0) {
    // handle error
}
```

### `wfb_tx_setup_radio(...)`

Updates the radiotap header (MCS / bandwidth / STBC / LDPC / GI / VHT) at runtime without
reinitialising the session. Mirrors `CMD_SET_RADIO` from the old `data_source()` command handler.
Returns 0 on success, -1 on error.

### `wfb_tx_setup_fec(k, n)`

Changes FEC parameters at runtime: flushes any open FEC block, reinitialises the session with
the new `k`/`n`, and resends session keys. Mirrors `CMD_SET_FEC`. Returns 0 on success, -1 on error.

### `wfb_tx_send(buf, size)`

Encrypts, FEC-encodes, and injects one payload packet. Handles the periodic session key
announcement (every `SESSION_KEY_ANNOUNCE_MSEC` = 1000 ms). Call once per RTP packet from
the encoder output path. No-op if `wfb_tx_init()` has not been called.

### `wfb_tx_destroy()`

Tears down the `RawSocketTransmitter` and releases all resources.

## Build

### Prerequisites

Run once before the first `make build` to cross-compile libsodium:

```sh
./wfb/build_wfb_tx.sh
```

This produces `wfb/build/sodium-install/lib/libsodium.a` used by the venc link step.
The script also builds the standalone `wfb_tx` binary with SHM input support (separate use case).

### Makefile integration

The wfbtx module is compiled and linked automatically as part of `make build`.

```
# C++ sources (gnu++11)
src/wfbtx/wifibroadcast.cpp  →  out/<soc>/wfbtx/wifibroadcast.o
src/wfbtx/tx_core.cpp        →  out/<soc>/wfbtx/tx_core.o
src/wfbtx/wfb_tx.cpp         →  out/<soc>/wfbtx/wfb_tx.o

# C source (gnu99, SIMD FEC)
src/wfbtx/zfex.c             →  out/<soc>/wfbtx/zfex.o

# Linked into venc via CC + -lstdc++ + static libsodium
```

NEON SIMD defines applied to zfex on star6e:
`-DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD -mfpu=neon-vfpv4`

### Supported targets

| `SOC_BUILD` | Toolchain | ABI |
|-------------|-----------|-----|
| `star6e` (default) | `arm-openipc-linux-gnueabihf-g++` | armv7-hf |
| `maruko` | `arm-openipc-linux-musleabihf-g++` | armv7-hf (musl) |

```sh
make build                     # star6e
make build SOC_BUILD=maruko    # maruko
```

## Key file

Default path: `/etc/drone.key`

Override with `-K /path/to/key` in the argv passed to `wfb_tx_init()`.

Generate a keypair on the device:

```sh
wfb_keygen /etc/drone.key
```
