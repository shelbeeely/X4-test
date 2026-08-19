// Xteink X4 hardware bring-up / debug firmware.
//
// No SD card, no FreeInkUI. Boots straight into a button-driven on-device
// test menu (UP/DOWN select, CONFIRM run, BACK exits a running test) that
// exercises the display and button hardware from several different angles:
// the original border/corner/diagonal/text pattern, a checkerboard for
// ghosting/moire, a FULL/HALF/FAST refresh-mode timing comparison, a
// displayWindow() partial-refresh test, a live button/ADC readout screen,
// and the original continuous black/white flash-stress loop. Button
// press/release edges (and an idle raw-ADC heartbeat) are logged to serial
// regardless of which screen is active. Monitor at 115200 baud.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <InputManager.h>

#include <cctype>
#include <cstdio>

using namespace BoardConfig;

static EInkDisplay* display = nullptr;
static InputManager buttons;

// ---------------------------------------------------------------------------
// Minimal direct-framebuffer drawing helpers.
//
// FreeInkDisplay's public API only pushes pre-formed image data (drawImage)
// or clears the whole buffer (clearScreen) — it has no shape/text primitives
// (those live in FreeInkUI, a full UI layer this firmware intentionally
// doesn't pull in). So everything below is drawn straight into the raw 1bpp
// framebuffer: 0 = black, 1 = white (matches clearScreen's 0xFF-is-white
// default and the SDK's icon-format convention), MSB-first, row-major.
// ---------------------------------------------------------------------------

static inline void setPixel(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= display->getDisplayWidth() || y >= display->getDisplayHeight()) return;
  uint8_t* fb = display->getFrameBuffer();
  uint32_t idx = static_cast<uint32_t>(y) * display->getDisplayWidthBytes() + (x / 8);
  uint8_t mask = 0x80 >> (x % 8);
  if (black) {
    fb[idx] &= ~mask;
  } else {
    fb[idx] |= mask;
  }
}

static void drawLine(int x0, int y0, int x1, int y1, bool black) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    setPixel(x0, y0, black);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// 5x7 bitmap font — uppercase letters + digits + space + colon, covering
// every string this firmware draws (menu labels, test screens, button
// names). Each row is a 5-bit pattern, MSB (bit 4) = leftmost column.
struct Glyph {
  char c;
  uint8_t rows[7];
};

static const Glyph kFont[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {':', {0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b00000}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111}},
    {'D', {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01111, 0b10000, 0b10000, 0b10011, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
};

static const Glyph& findGlyph(char c) {
  for (const auto& g : kFont) {
    if (g.c == c) return g;
  }
  return kFont[0];  // fall back to space
}

static void drawChar(int x, int y, char c, uint8_t scale) {
  const Glyph& g = findGlyph(static_cast<char>(toupper(static_cast<unsigned char>(c))));
  for (uint8_t row = 0; row < 7; row++) {
    for (uint8_t col = 0; col < 5; col++) {
      if (!(g.rows[row] & (0x10 >> col))) continue;
      for (uint8_t sy = 0; sy < scale; sy++) {
        for (uint8_t sx = 0; sx < scale; sx++) {
          setPixel(x + col * scale + sx, y + row * scale + sy, true);
        }
      }
    }
  }
}

static void drawText(int x, int y, const char* s, uint8_t scale) {
  int cx = x;
  for (; *s; s++) {
    drawChar(cx, y, *s, scale);
    cx += 6 * scale;  // 5px glyph + 1px space, scaled
  }
}

static void drawNumber(int x, int y, int value, uint8_t scale) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%d", value);
  drawText(x, y, buf, scale);
}

static void dumpBoardConfig() {
  const BoardProfile& p = ACTIVE;
  const DisplayPins& d = p.display;

  Serial.println();
  Serial.println("==================== BoardConfig::ACTIVE ====================");
  Serial.printf("  board name          : %s\n", p.name);
  Serial.printf("  displayController   : %u\n", static_cast<unsigned>(p.displayController));
  Serial.printf("  displayWidth        : %u\n", p.displayWidth);
  Serial.printf("  displayHeight       : %u\n", p.displayHeight);
  Serial.printf("  displaySpiHz        : %lu\n", static_cast<unsigned long>(p.displaySpiHz));
  Serial.println("  --- display pins (BoardConfig::ACTIVE.display) ---");
  Serial.printf("    sclk              : %d\n", d.sclk);
  Serial.printf("    mosi              : %d\n", d.mosi);
  Serial.printf("    cs                : %d\n", d.cs);
  Serial.printf("    dc                : %d\n", d.dc);
  Serial.printf("    rst               : %d\n", d.rst);
  Serial.printf("    busy              : %d\n", d.busy);
  Serial.printf("    powerEnable       : %d\n", d.powerEnable);
  Serial.printf("  orientation         : mirrorX=%d mirrorY=%d\n", p.orientation.mirrorX, p.orientation.mirrorY);
  Serial.printf("  viewableInsets      : top=%u right=%u bottom=%u left=%u\n", p.viewableInsets.top,
                p.viewableInsets.right, p.viewableInsets.bottom, p.viewableInsets.left);
  Serial.printf("  power.latch0        : %d\n", p.power.latch0);
  Serial.println("  --- input pins (BoardConfig::ACTIVE.input) ---");
  Serial.printf("    inputStyle        : %u (XteinkAdcLadder=%u)\n", static_cast<unsigned>(p.inputStyle),
                static_cast<unsigned>(InputStyle::XteinkAdcLadder));
  Serial.printf("    adcPin1 (Back/Confirm/Left/Right) : GPIO%d\n", InputManager::BUTTON_ADC_PIN_1);
  Serial.printf("    adcPin2 (Up/Down)                 : GPIO%d\n", InputManager::BUTTON_ADC_PIN_2);
  Serial.printf("    power   : GPIO%d  activeHigh=%d\n", p.input.power, p.input.powerActiveHigh);
  Serial.println("===============================================================");
}

static void logButtonSnapshot(const char* prefix) {
  InputManager::ButtonAdcSample g1, g2;
  buttons.readButtonAdc(g1, g2);
  Serial.printf("[BTN] %-7s g1(GPIO%d)=%-4d->%-8s g2(GPIO%d)=%-4d->%-8s power=%s\n", prefix, g1.pin, g1.raw,
                g1.button >= 0 ? InputManager::getButtonName(g1.button) : "none", g2.pin, g2.raw,
                g2.button >= 0 ? InputManager::getButtonName(g2.button) : "none",
                buttons.isPressed(InputManager::BTN_POWER) ? "PRESSED" : "released");
}

static void dumpDisplayGeometry() {
  Serial.println("==================== EInkDisplay geometry ====================");
  Serial.printf("  getDisplayWidth()      : %u\n", display->getDisplayWidth());
  Serial.printf("  getDisplayHeight()     : %u\n", display->getDisplayHeight());
  Serial.printf("  getDisplayWidthBytes() : %u\n", display->getDisplayWidthBytes());
  Serial.printf("  getBufferSize()        : %lu\n", static_cast<unsigned long>(display->getBufferSize()));
  Serial.printf("  MAX_BUFFER_SIZE        : %lu\n", static_cast<unsigned long>(EInkDisplay::MAX_BUFFER_SIZE));
  Serial.printf("  isX3Mode()             : %d\n", display->isX3Mode());
  Serial.printf("  supportsAsyncRefresh() : %d\n", display->supportsAsyncRefresh());
  Serial.println("================================================================");
}

static void drawTestPattern() {
  uint16_t w = display->getDisplayWidth();
  uint16_t h = display->getDisplayHeight();

  display->clearScreen(0xFF);  // white

  // Border frame, 3px thick.
  const int borderThickness = 3;
  for (int t = 0; t < borderThickness; t++) {
    for (int x = 0; x < w; x++) {
      setPixel(x, t, true);
      setPixel(x, h - 1 - t, true);
    }
    for (int y = 0; y < h; y++) {
      setPixel(t, y, true);
      setPixel(w - 1 - t, y, true);
    }
  }

  // Solid 40x40 corner blocks.
  const int block = 40;
  for (int y = 0; y < block; y++) {
    for (int x = 0; x < block; x++) {
      setPixel(x, y, true);
      setPixel(w - 1 - x, y, true);
      setPixel(x, h - 1 - y, true);
      setPixel(w - 1 - x, h - 1 - y, true);
    }
  }

  // Corner-to-corner diagonal.
  drawLine(0, 0, w - 1, h - 1, true);

  // Centered debug text.
  drawText(w / 2 - 90, h / 2 - 20, "XTEINK X4", 3);
  drawText(w / 2 - 70, h / 2 + 10, "800X480", 3);
}

static void drawCheckerboard(int cell) {
  uint16_t w = display->getDisplayWidth();
  uint16_t h = display->getDisplayHeight();
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      setPixel(x, y, ((x / cell) + (y / cell)) % 2 == 0);
    }
  }
}

static void waitForAnyButtonPress() {
  for (;;) {
    buttons.update();
    for (uint8_t i = InputManager::BTN_BACK; i <= InputManager::BTN_POWER; i++) {
      if (buttons.wasPressed(i)) return;
    }
    delay(10);
  }
}

// --- One-shot test screens (draw + refresh, log timing, wait for a press) --

static void runPatternTest() {
  Serial.println();
  Serial.println("=== TEST: Pattern (border/corners/diagonal/text) ===");
  drawTestPattern();
  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();
  Serial.printf("  FULL_REFRESH: %lu ms\n", static_cast<unsigned long>(t1 - t0));
  Serial.println("=== TEST complete: press any button to return to menu ===");
  waitForAnyButtonPress();
}

static void runCheckerboardTest() {
  Serial.println();
  Serial.println("=== TEST: Checkerboard (ghosting/moire check) ===");
  drawCheckerboard(20);
  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();
  Serial.printf("  FULL_REFRESH: %lu ms\n", static_cast<unsigned long>(t1 - t0));
  Serial.println("=== TEST complete: press any button to return to menu ===");
  waitForAnyButtonPress();
}

static void runRefreshCompareTest() {
  Serial.println();
  Serial.println("=== TEST: Refresh mode comparison (FULL/HALF/FAST) ===");
  drawCheckerboard(40);
  display->displayBuffer(EInkDisplay::FULL_REFRESH);

  struct ModeInfo {
    EInkDisplay::RefreshMode mode;
    const char* name;
  };
  const ModeInfo modes[] = {
      {EInkDisplay::FULL_REFRESH, "FULL"},
      {EInkDisplay::HALF_REFRESH, "HALF"},
      {EInkDisplay::FAST_REFRESH, "FAST"},
  };
  bool black = true;
  for (const auto& m : modes) {
    black = !black;
    display->clearScreen(black ? 0x00 : 0xFF);
    uint32_t t0 = millis();
    display->displayBuffer(m.mode);
    uint32_t t1 = millis();
    Serial.printf("  %-4s refresh: %lu ms\n", m.name, static_cast<unsigned long>(t1 - t0));
    delay(500);
  }
  Serial.println("=== TEST complete: press any button to return to menu ===");

  display->clearScreen(0xFF);
  drawText(40, 60, "REFRESH TEST DONE", 3);
  drawText(40, 110, "SEE SERIAL LOG", 3);
  drawText(40, 200, "PRESS BUTTON", 3);
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  waitForAnyButtonPress();
}

static void runPartialWindowTest() {
  Serial.println();
  Serial.println("=== TEST: Partial window update (displayWindow) ===");
  uint16_t w = display->getDisplayWidth();
  uint16_t h = display->getDisplayHeight();

  display->clearScreen(0xFF);
  drawText(40, 40, "PARTIAL WINDOW TEST", 3);
  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();
  Serial.printf("  base FULL_REFRESH: %lu ms\n", static_cast<unsigned long>(t1 - t0));

  // Toggle a centered box a few times using displayWindow() only, so a
  // failure specific to the windowed-update path (vs. full-frame refresh)
  // is isolated.
  uint16_t boxW = 200, boxH = 200;
  uint16_t boxX = (w - boxW) / 2;
  uint16_t boxY = (h - boxH) / 2;
  for (int i = 0; i < 3; i++) {
    bool black = (i % 2) == 0;
    for (int y = boxY; y < boxY + boxH; y++) {
      for (int x = boxX; x < boxX + boxW; x++) {
        setPixel(x, y, black);
      }
    }
    uint32_t wt0 = millis();
    display->displayWindow(boxX, boxY, boxW, boxH);
    uint32_t wt1 = millis();
    Serial.printf("  displayWindow() pass %d: %lu ms\n", i, static_cast<unsigned long>(wt1 - wt0));
    delay(500);
  }
  Serial.println("=== TEST complete: press any button to return to menu ===");
  waitForAnyButtonPress();
}

// --- Continuous test screens (own loop, BACK exits) -------------------------

static void drawButtonLiveScreen(const InputManager::ButtonAdcSample& g1, const InputManager::ButtonAdcSample& g2,
                                  bool powerPressed, const char* lastEvent) {
  display->clearScreen(0xFF);
  int y = 30;
  drawText(40, y, "BUTTON TEST", 3);
  y += 60;

  drawText(40, y, "G1:", 3);
  drawNumber(140, y, g1.raw, 3);
  drawText(260, y, g1.button >= 0 ? InputManager::getButtonName(g1.button) : "NONE", 3);
  y += 40;

  drawText(40, y, "G2:", 3);
  drawNumber(140, y, g2.raw, 3);
  drawText(260, y, g2.button >= 0 ? InputManager::getButtonName(g2.button) : "NONE", 3);
  y += 40;

  drawText(40, y, powerPressed ? "PWR:ON" : "PWR:OFF", 3);
  y += 60;

  drawText(40, y, "LAST:", 3);
  drawText(160, y, lastEvent, 3);
  y += 80;

  drawText(40, y, "BACK TO EXIT", 3);
}

static void runButtonLiveTest() {
  Serial.println();
  Serial.println("=== TEST: Live button/ADC readout (BACK to exit) ===");

  char lastEvent[24] = "NONE";
  InputManager::ButtonAdcSample g1, g2;
  buttons.readButtonAdc(g1, g2);
  drawButtonLiveScreen(g1, g2, buttons.isPressed(InputManager::BTN_POWER), lastEvent);
  display->displayBuffer(EInkDisplay::FULL_REFRESH);

  uint32_t lastRedrawMs = millis();
  for (;;) {
    buttons.update();

    bool anyEdge = false;
    for (uint8_t i = InputManager::BTN_BACK; i <= InputManager::BTN_POWER; i++) {
      if (buttons.wasPressed(i)) {
        Serial.printf("[BTN] PRESS   %s\n", InputManager::getButtonName(i));
        logButtonSnapshot("->");
        snprintf(lastEvent, sizeof(lastEvent), "%s PRESS", InputManager::getButtonName(i));
        anyEdge = true;
      }
      if (buttons.wasReleased(i)) {
        Serial.printf("[BTN] RELEASE %s (held %lums)\n", InputManager::getButtonName(i),
                      static_cast<unsigned long>(buttons.getHeldTime()));
        logButtonSnapshot("->");
        snprintf(lastEvent, sizeof(lastEvent), "%s RELEASE", InputManager::getButtonName(i));
        anyEdge = true;
      }
    }

    if (buttons.wasPressed(InputManager::BTN_BACK)) {
      Serial.println("=== TEST complete: back to menu ===");
      return;
    }

    uint32_t now = millis();
    if (anyEdge || now - lastRedrawMs >= 2000) {
      buttons.readButtonAdc(g1, g2);
      drawButtonLiveScreen(g1, g2, buttons.isPressed(InputManager::BTN_POWER), lastEvent);
      display->displayBuffer(EInkDisplay::FAST_REFRESH);
      lastRedrawMs = now;
    }

    delay(10);
  }
}

static void runFlashStressTest() {
  Serial.println();
  Serial.println("=== TEST: Continuous black/white flash stress (BACK to exit) ===");

  bool black = false;
  bool first = true;
  uint32_t lastToggleMs = 0;

  for (;;) {
    buttons.update();
    if (buttons.wasPressed(InputManager::BTN_BACK)) {
      Serial.println("=== TEST complete: back to menu ===");
      return;
    }

    uint32_t now = millis();
    if (first || now - lastToggleMs >= 5000) {
      first = false;
      lastToggleMs = now;
      black = !black;

      int busyPin = ACTIVE.display.busy;
      Serial.println();
      Serial.printf("[FLASH] Refreshing to %s\n", black ? "BLACK" : "WHITE");
      Serial.printf("[FLASH] BUSY pin (GPIO%d) before refresh : %d\n", busyPin, digitalRead(busyPin));

      display->clearScreen(black ? 0x00 : 0xFF);
      uint32_t t0 = millis();
      display->displayBuffer(EInkDisplay::FULL_REFRESH);
      uint32_t t1 = millis();

      Serial.printf("[FLASH] BUSY pin (GPIO%d) after refresh  : %d\n", busyPin, digitalRead(busyPin));
      Serial.printf("[FLASH] Refresh duration                 : %lu ms\n", static_cast<unsigned long>(t1 - t0));
    }

    delay(10);
  }
}

// --- Menu --------------------------------------------------------------------

static const char* kMenuLabels[] = {"PATTERN", "CHECKER", "REFRESH", "PARTIAL", "BUTTONS", "FLASH"};
static constexpr uint8_t kMenuCount = sizeof(kMenuLabels) / sizeof(kMenuLabels[0]);
static uint8_t menuSelected = 0;

static void drawMenu(uint8_t selected) {
  display->clearScreen(0xFF);
  drawText(40, 20, "TEST MENU", 3);

  int y = 90;
  for (uint8_t i = 0; i < kMenuCount; i++) {
    if (i == selected) {
      // Filled cursor triangle to the left of the selected label.
      for (int dx = 0; dx < 10; dx++) {
        for (int dy = -dx; dy <= dx; dy++) {
          setPixel(50 + dx, y + 8 + dy, true);
        }
      }
    }
    drawText(80, y, kMenuLabels[i], 3);
    y += 50;
  }

  drawText(40, y + 20, "UP DOWN SELECT", 3);
  drawText(40, y + 60, "CONFIRM RUN", 3);
}

static void runSelectedTest(uint8_t index) {
  switch (index) {
    case 0:
      runPatternTest();
      break;
    case 1:
      runCheckerboardTest();
      break;
    case 2:
      runRefreshCompareTest();
      break;
    case 3:
      runPartialWindowTest();
      break;
    case 4:
      runButtonLiveTest();
      break;
    case 5:
      runFlashStressTest();
      break;
    default:
      break;
  }
}

// --- Global (always-on) button activity logging -----------------------------
// Runs every loop() iteration regardless of menu/test state so hardware
// button issues are visible on serial even while just sitting in the menu.

static bool logButtonEdges() {
  bool anyEdge = false;
  for (uint8_t i = InputManager::BTN_BACK; i <= InputManager::BTN_POWER; i++) {
    if (buttons.wasPressed(i)) {
      Serial.printf("[BTN] PRESS   %s\n", InputManager::getButtonName(i));
      logButtonSnapshot("->");
      anyEdge = true;
    }
    if (buttons.wasReleased(i)) {
      Serial.printf("[BTN] RELEASE %s (held %lums)\n", InputManager::getButtonName(i),
                    static_cast<unsigned long>(buttons.getHeldTime()));
      logButtonSnapshot("->");
      anyEdge = true;
    }
  }
  return anyEdge;
}

static void logButtonHeartbeatIfIdle(bool anyEdge) {
  static uint32_t lastHeartbeatMs = 0;
  uint32_t now = millis();
  if (!anyEdge && now - lastHeartbeatMs >= 2000) {
    logButtonSnapshot("idle");
    lastHeartbeatMs = now;
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }
  delay(200);

  Serial.println();
  Serial.println("### Xteink X4 display debug firmware booting ###");

  // Battery-latched boards power off the instant USB is unplugged unless
  // this is asserted; a no-op on units that self-latch. Real board-bring-up,
  // not app logic — see BoardConfig::holdPowerRails().
  BoardConfig::holdPowerRails();

  dumpBoardConfig();

  const DisplayPins& d = ACTIVE.display;
  display = new EInkDisplay(d.sclk, d.mosi, d.cs, d.dc, d.rst, d.busy);
  display->begin();

  buttons.begin();

  dumpDisplayGeometry();

  Serial.println("Drawing test pattern (border + corner blocks + diagonal + text)...");
  drawTestPattern();

  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();
  Serial.printf("Initial FULL_REFRESH took %lu ms\n", static_cast<unsigned long>(t1 - t0));

  Serial.println("### Setup complete: entering test menu (UP/DOWN/CONFIRM) ###");
  drawMenu(menuSelected);
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
}

void loop() {
  buttons.update();

  bool anyEdge = logButtonEdges();
  logButtonHeartbeatIfIdle(anyEdge);

  if (buttons.wasPressed(InputManager::BTN_UP)) {
    menuSelected = (menuSelected == 0) ? (kMenuCount - 1) : (menuSelected - 1);
    drawMenu(menuSelected);
    display->displayBuffer(EInkDisplay::FAST_REFRESH);
  } else if (buttons.wasPressed(InputManager::BTN_DOWN)) {
    menuSelected = (menuSelected + 1) % kMenuCount;
    drawMenu(menuSelected);
    display->displayBuffer(EInkDisplay::FAST_REFRESH);
  } else if (buttons.wasPressed(InputManager::BTN_CONFIRM)) {
    runSelectedTest(menuSelected);
    drawMenu(menuSelected);
    display->displayBuffer(EInkDisplay::FULL_REFRESH);
  }

  delay(10);
}
