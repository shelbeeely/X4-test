# Xteink X4 display debug firmware

A barebones PlatformIO firmware project for debugging display issues on the
Xteink X4 (ESP32-C3 + SSD1677 800x480 e-paper), built on
[freeink-sdk](https://github.com/Free-Ink/freeink-sdk). It initializes the
display, dumps pin assignments and geometry over serial, draws a test
pattern, then loops toggling full black/white every 5 seconds while logging
BUSY pin state and refresh timing.

Everything builds in GitHub Actions — no local toolchain needed. Flash and
monitor from Chrome over WebSerial.

## Building

Every push to any branch triggers a build automatically. You can also
trigger one manually:

1. Go to the **Actions** tab of this repository.
2. Select the **Build firmware** workflow in the left sidebar.
3. Click **Run workflow** (uses the `workflow_dispatch` trigger).

## Downloading the build

1. Open the finished run under the **Actions** tab.
2. Scroll to **Artifacts** at the bottom of the run summary page.
3. Download `xteink_x4-firmware.zip` and unzip it. You'll get:
   - `firmware.bin` — the application image
   - `bootloader.bin` — the second-stage bootloader
   - `partitions.bin` — the partition table
   - `firmware.merged.bin` — bootloader + partition table + app combined into
     one flat image (see below for why this is the one you want)

## Flashing over WebSerial (Chrome, no local tools)

Use **[esptool-js](https://espressif.github.io/esptool-js)** or
**[ESPHome Web](https://web.esphome.io)** — both flash over WebSerial
directly from Chrome.

**Flash offset:** use `firmware.merged.bin` at offset **`0x0`**. It already
contains the bootloader (0x0), partition table (0x8000), and application
(0x10000) merged into one image (`platformio.ini` sets
`board_build.merge_bin = true` for exactly this reason), so you don't need to
juggle three separate offsets.

- **ESPHome Web** (simplest): open the site, click **Connect**, pick the
  X4's serial port, choose **Install** → **Install from file**, select
  `firmware.merged.bin`, and flash. ESPHome Web always writes the file it's
  given starting at `0x0`, which is correct for the merged image.
- **esptool-js**: open the site, connect to the port, add one file entry —
  `firmware.merged.bin` at offset `0x0` — and click **Program**.

If you'd rather flash the three files individually (e.g. `merge_bin` isn't
available for some reason and only the separate files exist in the
artifact), use esptool-js with three entries instead:

| File | Offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partitions.bin` | `0x8000` |
| `firmware.bin` | `0x10000` |

## Monitoring serial output over WebSerial

The X4's ESP32-C3 exposes its console over native USB (no separate
USB-UART bridge chip), so any WebSerial-based terminal works:

1. In **esptool-js**, after flashing, use its built-in **Console** tab, or
2. Open a WebSerial terminal such as the one built into ESPHome Web's
   **Logs** view, or a standalone tool like
   [Google's Serial Terminal](https://googlechromelabs.github.io/serial-terminal/).
3. Connect at **115200 baud**.
4. Reset the board (or replug USB) to see the boot log: the `BoardConfig`
   pin dump, display geometry, initial refresh timing, then a line every 5s
   as it toggles black/white, logging the BUSY pin state and refresh
   duration.

## Project layout

- `platformio.ini` — the `xteink_x4` build environment (ESP32-C3, SSD1677,
  single-buffer mode).
- `src/main.cpp` — the debug firmware itself.
- `freeink-sdk/` — [freeink-sdk](https://github.com/Free-Ink/freeink-sdk) as
  a git submodule; `platformio.ini`'s `lib_deps` point at
  `freeink-sdk/libs/hardware/BoardConfig` and
  `freeink-sdk/libs/display/FreeInkDisplay` via `symlink://`.
- `.github/workflows/build.yml` — builds on every push and on demand, and
  uploads the firmware binaries as a workflow artifact.
