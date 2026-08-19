# Xteink X4 display debug firmware

A barebones PlatformIO firmware project for debugging display and button
hardware issues on the Xteink X4 (ESP32-C3 + SSD1677/UC8179/UC8279
800x480 e-paper + ADC-ladder buttons), built on
[freeink-sdk](https://github.com/Free-Ink/freeink-sdk). At boot it probes
which panel-controller silicon the unit actually carries — X4-family units
ship **SSD1677, UC8179, or UC8279 depending on production batch**, all on
the same pinout — and promotes the driver to match *before* touching the
panel (see "Screen stuck on its last image?" below), dumps pin assignments
and geometry over serial, draws the original test pattern once, then boots
into an on-device, button-driven test menu (UP/DOWN to select, CONFIRM to
run, BACK to exit a running test) covering seven different hardware
exercises:

| Menu item | What it tests |
|---|---|
| `PATTERN` | The original border/corner-blocks/diagonal/text pattern, one `FULL_REFRESH` |
| `CHECKER` | A fine checkerboard, for spotting ghosting/moiré/dead rows or columns |
| `REFRESH` | The same image redrawn with `FULL_REFRESH`, `HALF_REFRESH`, and `FAST_REFRESH` back to back, timing each |
| `PARTIAL` | A base full-frame draw, then three `displayWindow()` partial updates on a centered box only |
| `BUTTONS` | A live on-screen readout of both button-ladder ADC groups, the power pin, and the last press/release, refreshed on activity or every ~2s |
| `PROBE` | Reviews the boot-time controller probe result (VER/FLG bytes, promotion verdict) without rebooting |
| `FLASH` | The original continuous black/white toggle every 5s, logging BUSY pin state and refresh timing |

Button press/release edges (and an idle raw-ADC heartbeat) are logged to
serial as `[BTN]` lines regardless of which screen is active, so a drifted
or flaky divider is visible even without a full press.

### Screen stuck on its last image?

That's the classic symptom of the display driver sending commands the panel
doesn't understand — most commonly because this specific unit shipped with
a different controller than the profile assumes. The firmware now probes
for this at every boot (`freeink::applyXteinkDisplayController()`) and logs
the result as `[XTDET]` lines: the raw `VER`/`FLG` register bytes, whether
an UltraChip (UC8179/UC8279) part was confirmed and the driver promoted, a
dump of the panel's factory MTP config (readable even when nothing is
visible on screen), and the OEM's own NVS-recorded panel type for
cross-reference. Check those lines first — either in the serial monitor or
via the `PROBE` menu item — before suspecting wiring. If the probe confirms
the default (SSD1677) controller and the screen still doesn't move, the
next things to check are: the FPC ribbon connecting the panel to the board
fully seated, and whether `BUSY` ever toggles at all during a refresh (the
`[LOOP]`/`[FLASH]`/test-screen serial lines report it before and after every
refresh call).

Everything builds in GitHub Actions — no local toolchain needed. Flash and
monitor from Chrome over WebSerial.

## Flashing and monitoring from the GitHub Pages site (easiest)

Every push builds the firmware and deploys a page to
**https://shelbeeely.github.io/X4-test/** with four independent tools, all
over WebSerial, the camera API, or client-side API calls — no drivers or
command line:

- **Flash** — a one-click install button (via
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/), the same widget
  behind ESPHome Web) that always flashes whatever the latest successful
  build produced. Nothing to download or unzip.
- **Serial monitor** — a live 115200-baud console built directly into the
  page (plain Web Serial API, independent of the flash button), so you can
  watch the debug output — display refresh logging and `[BTN]` button
  press/release/heartbeat lines — without any separate app.
- **Panel camera capture** — grabs a still from this device's camera (plain
  `getUserMedia`, nothing uploaded anywhere) so the AI assistant can check
  whether the physical panel visually matches what a test screen should
  show, not just what the firmware logged.
- **AI debug assistant** — paste an [OpenRouter](https://openrouter.ai/keys)
  API key (stored only in your browser's `localStorage`, sent straight to
  OpenRouter, never to any server of ours — this site is static) to send the
  current console log — and the captured photo, if you took one — to an LLM
  for a second opinion on hardware issues: stuck BUSY pin, odd refresh
  timing, ADC readings sitting on a classification boundary, missing
  events, a panel that looks blank/garbled/stuck compared to what the log
  says it should show, etc.

On a Chromebook or any Chrome/Edge browser:

1. Plug the X4 in over USB.
2. Open the Pages URL above.
3. Under **1. Flash**, click **Connect & Flash**, pick the serial port, and
   confirm.
4. Under **2. Watch the debug output**, click **Connect** (a separate port
   request — close the flash dialog first if it's still open), then reset
   the board to see the full boot log.
5. Under **3. Show the AI a photo of the panel**, click **Enable camera**,
   point it at the X4's screen, and **Capture photo** (optional — skip this
   step for a log-only analysis).
6. Under **4. Ask an AI about the log (and photo)**, save your OpenRouter
   key once, then click **Analyze log with AI** any time you want a read on
   what the console (and panel, if captured) is showing.

> **One-time repo setup:** GitHub Pages must be enabled once before the
> `Deploy to GitHub Pages` step in the workflow will succeed: go to
> **Settings → Pages → Build and deployment → Source** and select
> **GitHub Actions**. After that, every push deploys automatically.

If Web Serial or GitHub Pages isn't an option, use the manual download +
esptool-js/ESPHome Web flow below instead.

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
(0x10000) merged into one image — the workflow builds it with `esptool.py
merge_bin` right after `pio run` — so you don't need to juggle three
separate offsets.

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
4. Reset the board (or replug USB) to see the boot log: `[XTDET]` lines from
   the panel-controller probe (see "Screen stuck on its last image?" above),
   the `BoardConfig` pin dump (display and button pins), display geometry,
   and initial refresh timing, then `=== TEST: ... ===` lines whenever you
   run something from the on-device menu, interleaved with `[BTN]` lines on
   every button press/release (with the raw ADC readings for both button
   groups) and a heartbeat line roughly every 2s while idle.

## Project layout

- `platformio.ini` — the `xteink_x4` build environment (ESP32-C3, SSD1677,
  single-buffer mode).
- `src/main.cpp` — the debug firmware itself.
- `freeink-sdk/` — [freeink-sdk](https://github.com/Free-Ink/freeink-sdk) as
  a git submodule; `platformio.ini`'s `lib_deps` point at
  `freeink-sdk/libs/hardware/BoardConfig`,
  `freeink-sdk/libs/display/FreeInkDisplay`,
  `freeink-sdk/libs/hardware/InputManager`, and
  `freeink-sdk/libs/hardware/XteinkDetect` via `symlink://`.
- `.github/workflows/build.yml` — builds on every push and on demand, uploads
  the firmware binaries as a workflow artifact, and deploys the Pages
  flashing site.
- `web/` — the GitHub Pages flashing site (`index.html` is committed; the
  workflow generates `web/firmware/firmware.merged.bin` and `manifest.json`
  at build time — see `.gitignore`).
