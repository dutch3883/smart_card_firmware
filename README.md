# Smart Card Firmware

Auto-update target for the IdeaSpark RFID clock-in device
(spec 008 in [clocking_system](https://github.com/dutch3883/clocking_system)).

The device polls `latest.txt` once an hour and on every boot. When the
published version is greater than its compiled `FIRMWARE_VERSION`, the
device downloads the matching `firmware.bin` from the GitHub Release of
that tag and self-flashes via the ESP32 OTA mechanism.

## Layout

- `latest.txt` — one line, current published version (e.g. `v0.1.0`)
- GitHub Releases — each `vX.Y.Z` tag carries the matching `firmware.bin`

## Cutting a release

From the parent `clocking_system` working tree:
```
./scripts/release_firmware.sh v0.2.0 "release notes here"
```
