# USBLiter8 BootSurreal

## Overview

**USBLiter8 BootSurreal** is a standalone extension for `usbliter8` that allows an RP2350 board to automatically send a iBSS payload after successfully exploiting an Apple device into **PWNED DFU mode**.

Unlike the original workflow, this project does not require `usbliter8ctl` or a computer after the process starts.

The RP2350 acts as a complete USB host:

1. **Detects** Recovery or DFU mode devices automatically.
2. **Guides DFU entry** from Recovery mode using LED color cues (no computer needed).
3. Runs the **USBLiter8 exploit**.
4. Sends a pre-embedded `iBSS.boot` payload.
5. Executes the payload and **boots your surrealra1n downgraded device**.

---

## LED Guide

| LED Color | Meaning |
|-----------|---------|
| 🟠 Orange blink | Initializing, please wait |
| 🟠 Orange stable | Waiting for device |
| 🟣 Magenta/Purple | **Hold Volume Down + Power buttons** |
| 🔵 Cyan | **Release Power, keep holding Volume Down** |
| ⚪ White blink | Waiting for DFU mode entry |
| 🔵 Blue stable | Exploit in progress |
| 🟢 Green blink | Sending boot payload |
| 🟢 Green stable | Done! Device is booting |
| 🔴 Red | Error |

### DFU Entry Sequence

If the card detects your device in **Recovery Mode** (iTunes/cable icon on screen):

1. **Magenta** lights up — get ready, then **hold Volume Down + Power**
2. The card automatically sends a reboot command at the precise timing
3. **Cyan** lights up — **release Power**, keep holding Volume Down
4. After 8 seconds, **white blink** = waiting for DFU
5. Once DFU is detected, the exploit runs automatically

If your device is already in **DFU Mode** (black screen), the card skips straight to the exploit.

---

## How it works

```
Apple Device (Recovery or DFU Mode)
          |
          v
RP2350 USB Host
          |
          v
[If Recovery] DFU Helper (LED-guided entry)
          |
          v
USBLiter8 Exploit
          |
          v
PWNED DFU Mode
          |
          v
LZ4 decompress embedded .boot
          |
          v
DFU_DNLOAD payload transfer (0x80)
          |
          v
Zero-length DFU_DNLOAD → CUSTOM_BOOT → DFU_ABORT
          |
          v
Payload execution → Device boots!
```

---

## Building

### Supported systems

- Fedora
- Arch (CachyOS, etc.)
- macOS (Intel and Apple Silicon)
- Debian / Ubuntu

### Build steps

1. Place your `iBSS.boot` (from surrealra1n's `boot/` folder) into the `ibss/` directory.

2. Run:
```bash
./build.sh
```

3. The `.uf2` file will be in the `dist/` folder. Flash it to your Pico.

The build script automatically:
- Installs dependencies
- Downloads Pico SDK
- Downloads ARM GNU Toolchain
- Downloads USBLiter8 upstream source
- Patches in DFU helper + boot payload support
- Builds the final UF2 firmware

---

## Web Flasher

A static web-based tool is available in `web/` for generating UF2 files without building from source.

### Usage

1. Open `web/index.html` in a modern browser (or deploy to GitHub Pages)
2. Select your board
3. Upload your `iBSS.boot` file
4. Optionally upload a pre-compiled base firmware `.uf2`
5. Click "Generate & Download UF2"

The web flasher compresses and packages everything client-side — no server required.

---

## Supported boards

| Board | Status |
|-------|--------|
| waveshare_rp2350_usb_a | ✅ Tested |
| waveshare_rp2350_zero | 🔲 Untested |
| pimoroni_tiny2350 | 🔲 Untested |
| pico2 | 🔲 Untested |

> **Note:** Only RP2350-based boards are supported. RP2040 boards will NOT work.

---

## Current limitations

- Payload transfer uses 0x80 byte chunks (PIO USB hardware limit). Full transfer takes ~60-80 seconds depending on iBSS size.
- Supported boards have 2–4 MB of flash. One embedded `iBSS.boot` fills most of it, so only a single payload is supported.
- DFU helper requires the device to be in Recovery mode first (shows iTunes/cable icon). Normal mode is not supported — put the device in Recovery mode manually first.

---

## Credits

- **USBLiter8** exploit by the original author
- **SurrealBoot** by Validity (FogboundSloth25)
- **surrealra1n** by pwnerblu
- DFU helper, speed optimizations, and web flasher by BatuBey5G
