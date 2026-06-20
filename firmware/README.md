# firmware — IdeaSpark RFID clock-in device

Standalone ESP32 firmware for spec [007 RFID Card Clock-In System](../specs/007-rfid-card-clock/spec.md). Drives an OLED, a PN532 card reader, and a captive-portal WiFi flow. No laptop attached at runtime.

This README is the **developer-facing record of what was discovered during bring-up**. Treat it as the source of truth for pin assignments and hardware quirks; the spec describes *what* to build, this describes *how it actually works on this board*. Update it whenever a fact changes.

---

## Quickstart

```bash
cd firmware
pio run                                                          # build
python3 -m esptool --port /dev/cu.usbserial-140 --baud 460800 \
    write_flash -z 0x10000 .pio/build/esp32dev/firmware.bin      # flash
```

The serial port (`/dev/cu.usbserial-140`) changes when the cable moves to a different USB port — macOS derives the suffix from the USB location ID. Re-check with `ls /dev/cu.usbserial-*` after re-plugging.

Watch boot output (115200 baud):

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbserial-140', 115200, timeout=0.5)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 8
while time.time() < end:
    line = s.readline()
    if line: print(line.decode('utf-8', errors='replace').rstrip())"
```

A healthy boot ends with:

```
PN532 firmware 1.6
HTTP server up.
```

---

## Hardware identity

| Component | Identity | Confirmed via |
|-----------|----------|---------------|
| MCU | ESP32-D0WD-V3 (rev 3.1) | `esptool chip_id` at flash time |
| Flash | 8 MiB | `board_upload.flash_size = 8MB`, `default_8MB.csv` partition table |
| USB-serial | WCH CH340 (VID 0x1A86 / PID 0x7523) | macOS `system_profiler SPUSBDataType` |
| OLED | SSD1306 128×64 dual-color (yellow rows 0–15, blue rows 16–63, with a ~2 px physical inactive gap between zones) at I²C 0x3C | Bus scan + working frames |
| Card reader | Elechouse NFC Module V3 (PN532 chipset, firmware 1.6) | `getFirmwareVersion()` returned 1.6 |

---

## Pin assignments

Two independent buses on the ESP32. Each peripheral has its own bus — they do **not** share.

| Bus | Pins | Devices | Notes |
|-----|------|---------|-------|
| I²C (`Wire`) | SDA=GPIO 21, SCL=GPIO 22 | OLED at 0x3C | ESP32 Arduino default. Enables no-arg `Wire.begin()` + `Adafruit_SSD1306(&Wire)`. |
| SPI (`SPI`, default VSPI) | SCK=GPIO 18, MISO=GPIO 19, MOSI=GPIO 23, SS=GPIO 5 | PN532 | ESP32 Arduino default. Enables `Adafruit_PN532(SS, &SPI)`. |
| GPIO 2 | — | Heartbeat LED (1 Hz blink) | IdeaSpark silkscreen "L". |

**PN532 pins left disconnected**: IRQ (we poll), RSTO (chip has an internal reset pull-up).

**Power**:
- PN532: 3V3 rail. The chip draws 80–150 mA — too much for a GPIO (~40 mA max). Powering it from a GPIO browns it out.
- OLED: 3V3.
- ESP32: 5 V USB → onboard LDO regulates to 3V3.

---

## PN532 DIP switch configuration

The Elechouse v3 module has two DIP switches selecting the interface. **This build requires SPI**:

| Mode | SET0 | SET1 |
|------|------|------|
| HSU / UART | OFF | OFF |
| I²C | ON | OFF |
| **SPI (used here)** | **OFF** | **ON** |

If the switches are wrong, `getFirmwareVersion()` returns 0 and serial prints `PN532 not detected`.

---

## ESP32 strap pins — avoid these in future re-wires

Sampled at reset to decide boot mode. The current build does not interfere with them, but the constraints are:

| GPIO | Strap function | Used here? | Risk if disturbed |
|------|----------------|------------|-------------------|
| 0 | BOOT button | No | Must be high at boot |
| 2 | Boot mode + LED indicator | Heartbeat LED output | Must not be low at boot — pin is OUTPUT only after boot completes |
| 5 | SDIO timing | PN532 SS | Safe (SS-idle pull-up holds high) |
| 12 | Flash voltage select | No | Drives MTDI; must not be low at boot |
| 15 | SDIO output enable | No | Must not be low at boot |

---

## Hardware quirks ("lessons baked in")

Facts discovered during bring-up. These cost real time; do not regress them.

1. **PN532 must be on SPI**, not I²C, until pin headers are soldered onto the module. The Elechouse v3's bare-wire-in-PTH electrical contact is too marginal for I²C's open-drain signalling — 1-byte address handshakes work intermittently, but 9-byte command frames fail (`requestFrom() Error 263`). SPI's actively-driven push-pull signals tolerate the same wiring. The I²C frame-protocol code path has been removed from `main.cpp`.

2. **OLED + PN532 cannot share the I²C bus on this board.** Even when I²C wiring was momentarily solid, initialising the SSD1306 left the bus in a state the PN532 could not recover from, regardless of init order or retry policy. The dual-bus split (I²C for OLED, SPI for PN532) sidesteps this entirely.

3. **`SPI.begin(18, 19, 23, 5)` must be called explicitly** before `nfc.begin()`. The no-arg `SPI.begin()` does not always enable VSPI on Arduino-ESP32.

4. **First `getFirmwareVersion()` call sometimes returns 0.** Retry 5× with 100 ms gap. Subsequent reads are reliable.

5. **WiFiManager must be a global**, not stack-local in `setup()`. Non-blocking mode requires `wm.process()` to be called from `loop()`, and a stack-local instance is destructed before `loop()` ever runs.

6. **Captive portal needs `setConfigPortalTimeout(0)`** or the AP shuts down 3 minutes into setup and the device "disappears" from phone WiFi scans — looks like a crash.

7. **OLED contrast must be `0xFF`** (register `SSD1306_SETCONTRAST`) and **clock-div register `0xD0`** (`SSD1306_SETDISPLAYCLOCKDIV` — freq=13, divider=÷1, giving ~127 Hz frame rate) for QR codes to scan reliably under a phone camera. Library defaults look fine to the eye but produce beat-frequency aliasing under typical 30/60 FPS camera shutters that scrambles QR scans. 127 Hz is prime — not a harmonic of common shutter speeds.

8. **macOS CH340 driver can wedge** mid-session — the USB device is enumerated but `/dev/cu.usbserial-*` does not appear. Unplug-replug usually fixes it. If not, a Mac reboot does.

9. **The serial port suffix changes when the USB port changes** — macOS encodes the USB location ID into the name (e.g. `/dev/cu.usbserial-110` vs `-140`). `platformio.ini` pins it to one port; update both `upload_port` and `monitor_port` if you move cables.

---

## Bring-up sequence (current `main.cpp`)

1. `Serial.begin(115200)` + 300 ms grace.
2. `pinMode(LED_PIN, OUTPUT)` for heartbeat.
3. `Wire.begin()` (default 21/22), then `oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)`. Bail with halting LED blink if false.
4. Apply OLED contrast = 0xFF, clock-div = 0xD0.
5. `oledShowConnecting()` so the operator sees something during WiFi negotiation.
6. WiFiManager `autoConnect()` (non-blocking). On success → `enterStaMode()`. On failure → AP mode with the dual-QR alternation loop in `loop()`.
7. Inside `enterStaMode()`:
   1. `SPI.begin(18, 19, 23, 5)` then `delay(100)`.
   2. `nfc.begin()` then `delay(100)`.
   3. `nfc.getFirmwareVersion()` × up to 5 retries × 100 ms.
   4. `nfc.SAMConfig()`.
   5. Build marquee text, render first frame.
   6. Start HTTP server on port 80.

---

## Display behaviour

| State | Yellow zone (rows 0–15) | Blue zone (rows 16–63) | Held |
|-------|-------------------------|------------------------|------|
| Booting | "IdeaSpark Clock-In" header | "Connecting" + uptime | until WiFi attempt completes |
| AP setup (text screen) | "Setup mode (AP)" header | SSID, captive-portal URL | 8 s |
| AP setup (dual-QR screen) | `JOIN / WiFi` and `OPEN / page` labels | left QR = `WIFI:T:nopass;S:<ssid>;;`, right QR = `http://192.168.4.1`. Both QR v3 (29×29), 1 px/module, ECC low. | 8 s, then alternates with text |
| STA connected (default) | Right-to-left marquee `WiFi: <ssid>   IP: <ip>` at 2 px / 60 ms (~33 px/s), double-buffered for seamless wrap | "IdeaSpark Clock-In" header, divider at y=28, footer `Tap card / RFID off  RSSI<n>` | until a card tap |
| Card tap | (cleared) | "Card detected" + UID at text size 2 + `len=<n> hold <s>s` countdown | 5 s, then return to marquee |

Same-UID re-reads inside a 1.5 s dedup window are silently ignored.

---

## Networking

- **WiFiManager v2.0.17** (tzapu) — non-blocking captive portal.
- AP SSID: `ClockIn-Setup-XXXX` where `XXXX = (efuse_mac >> 32) & 0xFFFF` in hex. Stable per device.
- AP is **password-less** so phones can join without a credentials prompt.
- HTTP server (port 80, STA only):
  - `GET /` → HTML status page (SSID, IP, MAC, RSSI, uptime, RFID-ready flag).
  - `GET /reset` → clears WiFi credentials, restarts. Used to re-onboard at a new venue.
  - Everything else → 404.
- No mDNS / Bonjour. The OLED marquee surfaces the IP for browser entry.

---

## Storage (planned, not yet implemented)

8 MiB flash partitioned via `default_8MB.csv`:

- **NVS** → WiFi credentials (handled by WiFiManager), card→employee registrations.
- **LittleFS** → append-only `/events.log`, rotated daily.

The event log shape matches the existing `_ลงเวลา` row schema so the future Sheets sync (out of scope here) is a straight copy.

---

## Current status

| Stage | Status |
|-------|--------|
| 1–3: ESP32 boot, OLED, WiFi captive portal | ✅ done |
| 4: PN532 over I²C | ❌ abandoned — wiring too marginal, see quirk #1 |
| 5: shared-bus integration | ❌ abandoned — see quirk #2 |
| 6: PN532 over SPI | ✅ done |
| 7: Card-tap → 5 s UID display → marquee | ✅ done |
| 8 (spec 008): HTTPS POST /tap with bearer auth | ✅ code done; smoke test pending hardware |
| 9 (spec 008): LittleFS event queue + offline drain | ✅ code done; smoke test pending hardware |
| 10 (spec 008): registration page + /api/latest-tap + relays | ✅ code done; smoke test pending hardware |
| 11 (spec 008): sync health on OLED + home page | ✅ code done; smoke test pending hardware |

After hardware smoke-test passes:

| Stage | Status |
|-------|--------|
| 12: Multi-card-per-employee | ⏳ deferred |
| 13: Multi-device deployment | ⏳ deferred (per-device tokens) |

---

## File layout

- `platformio.ini` — board, libs, partition table, serial port.
- `src/main.cpp` — entire firmware in one file.
- `.gitignore` — keeps `.pio/` build artifacts out of git.
