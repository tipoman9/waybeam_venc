# Majestic Reverse Engineering — Star6C / I6C / SSC377QE

---

## CORRECTION: Wrong binary used in initial RE

**The disassembly below is from the SSC338Q (Infinity6B) squashfs, NOT from the
I6C (SSC377QE / Infinity6C) device.**  The SSC338Q majestic uses VPE; the I6C
majestic uses SCL.  All conclusions about VPE, FRAMEBASE+compress=0, and absent
MI_SCL_* functions in the sections below apply to SSC338Q and are WRONG for I6C.

**See the "I6C majestic proc dump RE" section immediately below for the correct data.**

---

## I6C majestic reverse engineering (proc dump method, confirmed on device)

This section documents the actual I6C (SSC377QE) majestic pipeline recovered by
reading `/proc/mi_modules` while majestic runs on the camera hardware.

### Pipeline topology

```
VIF (dev=0, chn=0, port=0)
  ↓  REALTIME (4)
ISP (dev=0, chn=0, port=0)
  ↓  REALTIME (4)
SCL (chn=0)
  └─ port 0 ─→ VENC (chn=0)   RING (16)
```

### VIF configuration

```
VIF dev: Atom=1, Fps=44.41, incrop=(0,0,2592,1944)
```

Identical to waybeam — VIF is not the differentiator.

### ISP configuration

```
ISP CHN: InSize=2592×1944, Crop=(0,0,2592,1944), pixel=RG_10BPP
         3DNRLevel=2, HdrMode=0, Mode=32, Compress=0 (output)
ISP dev: VsyncCnt=474, FrameDoneCnt=473, DropCnt=0, fifofullcnt=0
ISP output port: bindtype=Real, Pixel=YUV420SP, Compress=0,
                 PortCrop=(0,0,2592,1944), fps=44.40
CMDQ: TotalKickoff=238, Idle=1, CmdqISRCnt=236, DevISRCnt=236
```

ISP MMA buffers allocated (3DNR reference frames):
```
DNR0_INFO1  0x4fbc0 bytes   (~319 KB)
DNR0_INFO0  0x1338c0 bytes  (~1.2 MB)   ← temporal NR reference frame
DNR0_INFO2  0x48be80 bytes  (~4.5 MB)
```

CMDQ Idle=1/238 means the 3A+IQ system writes to ISP registers on nearly every frame.
DevISRCnt≈TotalKickoff — one ISP device interrupt per frame (single-pass processing).

### SCL configuration

```
SCL CHN: Crop=(0,0,2592,1944), no private MMA pool (current_buf_size=0)
SCL output port 0:
  bindtype = Ring (16)
  Compress = 6  (IFC)
  PortCrop = (0,0,2592,1944)
  OutputW = 1920, OutputH = 1080
  FPS = 44.40, DropCnt = 0
```

### VENC

```
VENC bind_type=16 (RING from mi_scl), fps=44, kbps=3532, DropCnt=0
```

### What majestic does NOT do (confirmed via proc + string table analysis)

- `MI_VENC_CreateDev` — kernel auto-creates device
- `MI_VENC_SetInputSourceConfig` — VENC stays in default NORMAL mode
- `MI_SYS_ConfigPrivateMMAPool` for SCL — no private SCL ring pool
- Any `MI_SYS_SetChnOutputPortDepth` on video ports

### Key difference: CMDQ activity vs waybeam

| Parameter | Majestic | Waybeam (after test 13) |
|-----------|----------|--------------------------|
| CMDQ TotalKickoff | 238 | 618 |
| CMDQ Idle | 1 (0.4%) | 616 (99.7%) |
| CMDQ CmdqISRCnt | 236 | ~2 |
| CMDQ DevISRCnt | 236 (≈1×/frame) | 1232 (≈2×/frame) |

Majestic's CMDQ is busy almost every frame (IQ register updates from 3A_Proc_0).
Waybeam's CMDQ is almost always idle — strongly correlated with the disabled
`MI_ISP_IQ_ApiCmdLoadBinFile` call (which initializes the CMDQ-backed IQ subsystem).
The 2× DevISRCnt in waybeam implies the ISP performs 2 DMA passes per frame, halving
effective throughput and causing FIFO FULL at 2592×1944@45fps.

---

## Legacy RE (SSC338Q binary — WRONG SoC, kept as reference)

**The following data is from the SSC338Q squashfs majestic binary.  It applies to
SSC338Q (Infinity6B) only and is incorrect for SSC377QE (Infinity6C / I6C).  Kept
for reference because it documents VPE-based pipeline architecture.**

---

**Binary**: `/home/home/src/majestics/squashfs-root/usr/bin/majestic`  
**Format**: 32-bit ARM Thumb ELF, PIE, dynamically linked, stripped (section headers present, no local symbol table)  
**Toolchain used**: `arm-openipc-linux-musleabihf-objdump -d` (cross-toolchain; capstone in Thumb mode misreads ARM32 PLT stubs)

---

### Libraries

Majestic links: `libmi_vif.so`, `libmi_isp.so`, **`libmi_vpe.so`**, `libmi_venc.so`, `libmi_sys.so`, `libmi_ai.so`, `libmi_ao.so`, `libmi_sed.so`, `libmi_ipu.so`, `libmi_ive.so`, `libmi_vdf.so`, `libmi_rgn.so`, `libmi_divp.so`, `libmi_shadow.so`, `libcus3a.so`, `libispalgo.so`, `libmi_sensor.so`, `libcam_os_wrapper.so`, `libcam_fs_wrapper.so`, `libmi_iqserver.so`, `libMD_LINUX.so`, `libjson-c.so.5`, `libopus.so.0`, `libmbedtls.so.13`, `libevent_core-2.2.so.1`, `libm.so.6`, `libc.so.6`.

**This binary uses `libmi_vpe.so` which is absent from I6C camera filesystems.**
On I6C, only `libmi_scl.so` is available — the I6C majestic uses SCL, not VPE.

---

### PLT addresses — SSC338Q binary (key video pipeline functions)

| Symbol | PLT addr |
|---|---|
| `MI_VPE_CreateChannel` | 0x8078 |
| `MI_VPE_SetPortMode` | 0x7cc0 |
| `MI_VPE_SetChannelRotation` | 0x7d3c |
| `MI_VPE_StartChannel` | 0x8c08 |
| `MI_VPE_EnablePort` | 0x8c38 |
| `MI_VENC_CreateChn` | 0x820c |
| `MI_VENC_StartRecvPic` | 0x7af0 |
| `MI_SYS_BindChnPort2` | 0x8470 |
| `MI_SYS_SetChnOutputPortDepth` | 0x88d8 |
| `MI_SYS_GetChnOutputPortDepth` | 0x7e94 |
| `MI_SED_CreateChn` | 0x8ecc |
| `MI_SED_AttachToVencChn` | 0x90ac |
| `MI_SED_StartDetector` | 0x7f1c |

**Absent from PLT** (confirmed not used):

- `MI_SYS_ConfigPrivateMMAPool` — no ring pools at all
- `MI_VENC_CreateDev` — kernel auto-creates the device on `CreateChn`
- `MI_VENC_SetInputSourceConfig` — VENC defaults to NORMAL frame input
- `MI_SCL_*` — no SCL functions whatsoever

---

### Pipeline topology — SSC338Q

```
VIF (module=6)
  ↓  REALTIME bind
ISP (module=5)
  ↓  (implicit ISP→VPE connection via VPE channel attr eSensorBindId)
VPE (module=7)  ←— CreateChannel + StartChannel
  ├─ port 0 or 1 ─→ VENC (module=2)   FRAMEBASE bind
  └─ port 2      ─→ SED               (motion/scene detection)
```

Audio: VIF→AI→VENC (separate path, not detailed here).

---

### Call sequence and disassembly findings — SSC338Q

#### 1. VIF→ISP bind (0x108ee)

```
MI_SYS_BindChnPort2(
    src = {module=6 (VIF), dev=0, chn=0, port=0},
    dst = {module=7 (VPE), dev=0, chn=0, port=0},
    srcFps = global_state[332],
    dstFps = global_state[332],
    eBindType = REALTIME (4),
    u32BindParam = 0
)
```

Note: `module=7` in this bind is VPE (not ISP). The ISP feeds VPE via the VPE channel's `eSensorBindId` attribute, not a separate bind. The VIF→VPE bind is what the SDK uses.

---

### 2. VPE CreateChannel (0x10742)

Called with `chn=0`. Channel attr struct (192 bytes, zeroed before filling):

| Struct offset | Value | Meaning |
|---|---|---|
| 0 | `u16` width from `global_state[100]` | `u16MaxW` |
| 2 | `u16` height from `global_state[104]` | `u16MaxH` |
| 4 | `global_state[320]` (4-byte store) | `ePixFmt` (likely YUV420SP=11) |
| 12 | 1 | `eSensorBindId` = 1 (or `bNrEn`; struct layout varies from SSAE header) |
| 24 | 24 = 0x18 | `eRunningMode` = `E_MI_VPE_RUN_REALTIME_MODE` (REALTIME_TOP\|REALTIME_BOTTOM) |

The `eRunningMode = 0x18` is the most important field. This sets VPE to realtime priority mode, which is what allows it to handle high-fps sensor input without stalling.

---

### 3. VPE SetChannelRotation (0x10870)

```
MI_VPE_SetChannelRotation(chn=0, rotation=global_state[328])
```

Called before `VPE_StartChannel`.

---

### 4. VPE StartChannel (0x10894)

```
MI_VPE_StartChannel(chn=0)
```

---

### 5. VPE SetPortMode + EnablePort — first batch (0x10944, 0x10976)

Runs in a loop over output ports (0 and/or 1) destined for VENC:

```
port_mode = {
    u16Width  = global_state[100],   // sensor/encode width
    u16Height = global_state[104],   // sensor/encode height
    [8]       = 11,                  // ePixelFormat = YUV420SP
    eCompressMode = 0                // NONE (from memset)
}
MI_VPE_SetPortMode(chn=0, port=<loop_var>, &port_mode)
MI_VPE_EnablePort(chn=0, port=<loop_var>)
```

**eCompressMode = 0 = NONE** — VPE outputs raw YUV420SP, no IFC compression. This is critical: FRAMEBASE binding requires uncompressed frames.

---

### 6. VENC CreateChn (0x10a98) and StartRecvPic (0x10b06)

Runs in a loop `r4 = 0, 1, 2` (up to 3 VENC channels). At `r4==2`, the standard path executes:

VENC channel attr struct (at `sp+128`, zeroed before filling):

| Struct offset | Value | Meaning |
|---|---|---|
| 0 | 4 | `eType` — codec type (H264=1? H265? actual value=4) |
| 4 | width | `u32MaxPicWidth` |
| 8 | height | `u32MaxPicHeight` |
| 16 | 1 (byte) | some flag |
| 40 | 6 | some enum (RC mode or profile) |
| 44 | computed | bitrate in bits/s: `((clamp(input,10,80)-10) * 7168 / 70 + 1024) * 1024` |

```
MI_VENC_CreateChn(chn=r4, &attr)   // on success, r0==0
MI_VENC_StartRecvPic(chn=r4)
```

`MI_VENC_CreateDev` is **not called** — the SDK creates the device automatically when `CreateChn` is called.  
`MI_VENC_SetInputSourceConfig` is **not called** — VENC defaults to NORMAL frame-based input mode.

---

### 7. VPE→VENC bind (0x10b70)

```
MI_SYS_BindChnPort2(
    src = {module=7 (VPE), dev=0, chn=0, port=<computed_per_channel>},
    dst = {module=2 (VENC), dev=0, chn=<loop_var>, port=0},
    srcFps = global_state[332],
    dstFps = [r8+4]  (per-channel fps),
    eBindType = FRAMEBASE (1),      ← stack[0] = 1
    u32BindParam = 0
)
```

**`eBindType = FRAMEBASE (1)`** — this is the key. VPE→VENC uses FRAMEBASE, not RING. FRAMEBASE passes whole decoded frames as buffers; no IFC-compressed DMA ring is involved.

VPE port → VENC channel mapping (per-channel, computed from loop var `r4`):
- `r4=0`: VPE port 1 → VENC chn 0
- `r4=1`: VPE port 0 → VENC chn 1
- `r4=2`: VPE port 1 → VENC chn 2

---

### 8. VPE SetPortMode + EnablePort — second batch (0x110ca, 0x1110a)

```
port_mode = {
    [0]  = <PC-relative constant>,   // output resolution (SED input size)
    [8]  = 11,                       // ePixelFormat = YUV420SP
    eCompressMode = 0                // NONE
}
MI_VPE_SetPortMode(chn=0, port=2, &port_mode)
MI_VPE_EnablePort(chn=0, port=2)
```

Port 2 feeds SED (motion detection). Configured after the main VENC bind loop.

---

### 9. SED pipeline (0x1117e–0x111c4)

```
// SED channel attr (at sp+128):
// [0]  = 0x160 = 352  (width?)
// [4]  = 0x120 = 288  (height?)
// [8]  = 10           (some count)
// [12] = 1
// [16] = 7
// [28] = 2            (some mode enum)
// [52] = clamp(quality,-32,31) from config

MI_SED_CreateChn(chn=1, &attr)
MI_SED_AttachToVencChn(...)
MI_SED_StartDetector(...)
```

---

### 10. Audio pipeline (after SED, around 0x10bc0–0x10da6)

```
MI_AI_SetPubAttr(dev=0, &pub_attr)   // sample_rate, channels, bit_width, sound_mode
MI_AI_Enable(dev=0)
MI_AI_EnableChn(dev=0, chn=AI_channel)
MI_AI_SetVqeVolume(dev=0, chn, volume)
MI_SYS_SetChnOutputPortDepth(&ai_port, userDepth=2, hwDepth=current)
MI_AO_SetPubAttr(dev=0, &ao_attr)
```

`MI_SYS_SetChnOutputPortDepth` is called **only for AI (module_id=4)**, not for any video port. Default buffer depths are used throughout the video pipeline.

---

### What SSC338Q majestic does NOT do

**Note: these findings are for SSC338Q only.  The I6C majestic "does NOT do" list
appears in the I6C proc dump section above.**

| Thing not done | Implication |
|---|---|
| No `MI_SYS_ConfigPrivateMMAPool` | No ring pool of any kind |
| No `MI_VENC_CreateDev` | Kernel auto-creates device on CreateChn |
| No `MI_VENC_SetInputSourceConfig` | VENC stays in default NORMAL mode |
| No `MI_SCL_*` functions | SSC338Q uses VPE, not SCL |
| No ring pool type=0 (ENCODER_RING) | Not needed for FRAMEBASE |
| No `MI_SYS_SetChnMMAConf` | MMA heap allocation is automatic |
| No `SetChnOutputPortDepth` on video ports | Driver defaults apply for video |

---

### Key architectural difference (SSC338Q VPE vs I6C SCL)

**Note: this comparison is between the SSC338Q majestic (VPE-based) and waybeam on I6C
(SCL-based).  The real I6C majestic also uses SCL — see the I6C proc dump RE section above.**

| Aspect | SSC338Q Majestic (VPE) | I6C Majestic (SCL) | Waybeam on I6C (SCL) |
|---|---|---|---|
| Scaler | VPE (`libmi_vpe.so`) | SCL (`libmi_scl.so`) | SCL (`libmi_scl.so`) |
| ISP→scaler | VPE channel `eSensorBindId` | `BindChnPort2` ISP→SCL REALTIME | `BindChnPort2` ISP→SCL REALTIME ✓ |
| scaler→VENC | FRAMEBASE (1), compress=0 | **RING (16), compress=6 (IFC)** | RING (16), compress=6 ✓ |
| Ring pool | None | None (confirmed) | None ✓ |
| VENC input source | NORMAL (no call) | NORMAL (no call) | NORMAL (no call) ✓ |
| ISP 3DNR | — | Level=2 | Level=2 ✓ |
| CMDQ activity | — | Idle=1/238 (busy) | Idle=616/618 (idle) ✗ |
| DevISRCnt | — | 1×/frame | 2×/frame ✗ |
| FIFO FULL @ 45fps | — | None | Present ✗ |

---

### Confirmed fix from SSC338Q RE

Changing waybeam's SCL→VENC path from `RING + IFC + RING_DMA input source` to
`FRAMEBASE + compress=0 + no SetInputSourceConfig` fixed VENC encoding on Star6C
(test 9).  However, FRAMEBASE does not match the actual I6C majestic which uses RING+IFC.

The current investigation is why RING+IFC+3DNR=2 in waybeam produces FIFO FULL at
2592×1944@45fps while I6C majestic does not.  See star6c_pipeline_tests.md test 13.
