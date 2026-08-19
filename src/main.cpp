// Xteink X4 display bring-up / debug firmware.
//
// No SD card, no input handling, no UI layer. Just: init the display, dump
// BoardConfig pin assignments + geometry over serial, draw a test pattern
// with one FULL_REFRESH, then loop toggling full black/white every 5s while
// logging BUSY pin state and refresh timing. Monitor at 115200 baud.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>

using namespace BoardConfig;

static EInkDisplay* display = nullptr;

// ---------------------------------------------------------------------------
// Minimal direct-framebuffer drawing helpers.
//
// FreeInkDisplay's public API only pushes pre-formed image data (drawImage)
// or clears the whole buffer (clearScreen) — it has no shape/text primitives
// (those live in FreeInkUI, a full UI layer this firmware intentionally
// doesn't pull in). So the test pattern is drawn straight into the raw 1bpp
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

// Tiny 5x7 bitmap font — only the glyphs this firmware's two debug labels
// need ("XTEINK X4" / "800X480"). Each row is a 5-bit pattern, MSB (bit 4) =
// leftmost column.
struct Glyph {
  char c;
  uint8_t rows[7];
};

static const Glyph kFont[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'I', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
};

static const Glyph& findGlyph(char c) {
  for (const auto& g : kFont) {
    if (g.c == c) return g;
  }
  return kFont[0];  // fall back to space
}

static void drawChar(int x, int y, char c, uint8_t scale) {
  const Glyph& g = findGlyph(c);
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
  Serial.println("===============================================================");
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

  dumpDisplayGeometry();

  Serial.println("Drawing test pattern (border + corner blocks + diagonal + text)...");
  drawTestPattern();

  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();
  Serial.printf("Initial FULL_REFRESH took %lu ms\n", static_cast<unsigned long>(t1 - t0));

  Serial.println("### Setup complete — entering black/white toggle loop (5s interval) ###");
}

void loop() {
  static bool black = false;
  black = !black;

  int busyPin = ACTIVE.display.busy;

  Serial.println();
  Serial.printf("[LOOP] Refreshing to %s\n", black ? "BLACK" : "WHITE");
  Serial.printf("[LOOP] BUSY pin (GPIO%d) before refresh : %d\n", busyPin, digitalRead(busyPin));

  display->clearScreen(black ? 0x00 : 0xFF);

  uint32_t t0 = millis();
  display->displayBuffer(EInkDisplay::FULL_REFRESH);
  uint32_t t1 = millis();

  Serial.printf("[LOOP] BUSY pin (GPIO%d) after refresh  : %d\n", busyPin, digitalRead(busyPin));
  Serial.printf("[LOOP] Refresh duration                 : %lu ms\n", static_cast<unsigned long>(t1 - t0));

  delay(5000);
}
