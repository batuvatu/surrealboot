# surrealboot
surrealboot is a standalone, on-the-go hardware tether-booter for Apple A12 and A13 devices powered by Raspberry Pi RP2350 microcontrollers.

the tool combines the usbliter8 bootrom exploit with a DFU helper to help you enter DFU mode without a computer, and a payload sender to send your iBSS.boot file which boots your tether-downgraded iPhone on the go. for more information about tether-downgrading an A12/A13 device, please see [surrealra1n](https://github.com/pwnerblu/surrealra1n).


🌐 Ready to use tool can be found [here](https://batuvatu.github.io).

---

## LEDs and meaning

When powered on, connect your iPhone to the board's USB host port with a Lightning cable and follow the RGB LED states:

| LED Color | Stage | Action Required |
| :--- | :--- | :--- |
| 🟠 **Solid Orange** | Waiting for device | Waiting for iPhone. if it's already plugged in, get ready to press the hardware buttons. |
| 🟣 **Solid Magenta** | DFU Helper Stage 1 | Hold Volume Down + Power at the same time immediately after seeing the light. |
| 🔵 **Solid Cyan** | DFU Helper Stage 2 | Release power button, but do not release volume down. |
| ⚪ **Blinking White** | Waiting for DFU | You can let go of the button now it the board is stuck here, entering DFU failed. |
| 🔵 **Solid Blue** | Exploit Running | usbliter8 bootrom exploit executing. |
| 🟢 **Blinking Green** | Booting Device | Sending boot payload, this might take a minute.  |
| 🟢 **Solid Green** | Payload Sent | Payload sent, device should boot. |
| 🔴 **Solid Red** | Error | Transfer or exploit failed. Unplug and reconnect to retry. |



## Quick Start

1. Open the [surrealboot Web Flasher](https://batuvatu.github.io) in Google Chrome, Microsoft Edge, Opera or newer versions of Firefox. 
2. Select your RP2350 board and click the download button.
3. Hold the boot button on your board, plug it into your computer, and drag the downloaded .uf2 file to the appeared drive. 
4. Upload your device's iBSS.boot file. this file can be found on the boot folder inside the surrealra1n folder.
5. Click the flash button to flash the boot payload over Web Serial.



## Supported iDevices

Detailed information can be found on [surrealra1n wiki/Supported Devices](https://github.com/pwnerblu/surrealra1n/wiki/Supported-Devices)

### Supported RP2350 Microcontrollers
| Board | Status | Host Port / Data Pins |
| :--- | :--- | :--- |
| Waveshare RP2350 USB-A | ✅ Recommended (Plug & Play) | Onboard USB-A Female (GP12 / GP13) |
| Waveshare RP2350-Zero |  Untested | GP12 (+) / GP13 (-) |
| Pimoroni Tiny 2350 | Untested | GP16 (+) / GP17 (-) |
| Raspberry Pi Pico 2 | Untested | GP16 (+) / GP17 (-) |

---

## Building from Source

### Prerequisites
Supported on Linux (Fedora, Arch, Debian/Ubuntu) and macOS (Apple Silicon / Intel). MacOS is not tested yet. 

```bash
# Clone repository
git clone https://github.com/batuvatu/surrealboot.git
cd surrealboot

# (Optional) Place a default iBSS.boot in ibss/ to bake it directly into flash
cp /path/to/iBSS.boot ibss/iBSS.boot

# Build firmware for your board
PICO_BOARD=waveshare_rp2350_usb_a ./build.sh
```

The compiled UF2 firmware will be output to `dist/usbliter8-bootsurreal-waveshare_rp2350_usb_a.uf2`.



## Credits
- usbliter8 by Paradigm Shift
- BootSurreal by FogboundSloth25
- surrealra1n by pwnerblu
