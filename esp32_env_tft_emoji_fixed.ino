#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

static const int PIN_LIGHT = 34;
static const int PIN_TFT_SCLK = 18;
static const int PIN_TFT_MOSI = 23;
static const int PIN_TFT_CS = 5;
static const int PIN_TFT_DC = 27;
static const int PIN_TFT_RST = 26;
static const int PIN_TFT_BL = 14;
static const int PIN_SDA = 21;
static const int PIN_SCL = 22;
static const uint8_t SHT30_ADDR = 0x44;

static const int W = 172;
static const int H = 320;

SPIClass spi(VSPI);

static const int CARD_X = 12;
static const int CARD_W = 148;
static const int CARD_H = 92;
static const int ICON_ZONE_W = 36;
static const int ICON_GAP = 8;
static const int VALUE_LEFT = CARD_X + 12 + ICON_ZONE_W + ICON_GAP;
static const int VALUE_RIGHT = CARD_X + CARD_W - 10;
static const int VALUE_CENTER_X = (VALUE_LEFT + VALUE_RIGHT) / 2;


float luxValue = 0.0f;
float tempValue = 0.0f;
float humValue = 0.0f;
bool shtOk = false;
unsigned long lastLightMs = 0;
unsigned long lastShtMs = 0;
unsigned long lastDrawMs = 0;

uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void sendCmd(uint8_t v) {
  digitalWrite(PIN_TFT_DC, LOW);
  digitalWrite(PIN_TFT_CS, LOW);
  spi.transfer(v);
  digitalWrite(PIN_TFT_CS, HIGH);
}

void send8(uint8_t v) {
  digitalWrite(PIN_TFT_DC, HIGH);
  digitalWrite(PIN_TFT_CS, LOW);
  spi.transfer(v);
  digitalWrite(PIN_TFT_CS, HIGH);
}

void send16x2(uint16_t a, uint16_t b) {
  digitalWrite(PIN_TFT_DC, HIGH);
  digitalWrite(PIN_TFT_CS, LOW);
  spi.transfer(a >> 8);
  spi.transfer(a & 0xFF);
  spi.transfer(b >> 8);
  spi.transfer(b & 0xFF);
  digitalWrite(PIN_TFT_CS, HIGH);
}

void setWin(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  sendCmd(0x2A);
  send16x2(x + 34, x + w - 1 + 34);
  sendCmd(0x2B);
  send16x2(y, y + h - 1);
  sendCmd(0x2C);
}

void fillRectFast(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;

  setWin(x, y, w, h);
  uint8_t hi = color >> 8;
  uint8_t lo = color & 0xFF;
  digitalWrite(PIN_TFT_DC, HIGH);
  digitalWrite(PIN_TFT_CS, LOW);
  for (int i = w * h; i > 0; --i) {
    spi.transfer(hi);
    spi.transfer(lo);
  }
  digitalWrite(PIN_TFT_CS, HIGH);
}

void rectFast(int x, int y, int w, int h, uint16_t c) {
  fillRectFast(x, y, w, 1, c);
  fillRectFast(x, y + h - 1, w, 1, c);
  fillRectFast(x, y, 1, h, c);
  fillRectFast(x + w - 1, y, 1, h, c);
}

void initTft() {
  pinMode(PIN_TFT_CS, OUTPUT);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  digitalWrite(PIN_TFT_DC, HIGH);
  digitalWrite(PIN_TFT_BL, HIGH);

  spi.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  spi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));

  digitalWrite(PIN_TFT_RST, LOW);
  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH);
  delay(120);

  sendCmd(0x01); delay(150);
  sendCmd(0x11); delay(10);
  sendCmd(0x3A); send8(0x55); delay(10);
  sendCmd(0x36); send8(0xC0);
  sendCmd(0x21); delay(10);
  sendCmd(0x13); delay(10);
  sendCmd(0x29); delay(10);
}

uint8_t crc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool readSht(float& t, float& h) {
  Wire.beginTransmission(SHT30_ADDR);
  Wire.write(0x24);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(20);

  if (Wire.requestFrom((int)SHT30_ADDR, 6) != 6) return false;

  uint8_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = Wire.read();

  if (crc8(d, 2) != d[2]) return false;
  if (crc8(d + 3, 2) != d[5]) return false;

  uint16_t rt = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rh = ((uint16_t)d[3] << 8) | d[4];

  t = -45.0f + 175.0f * ((float)rt / 65535.0f);
  h = 100.0f * ((float)rh / 65535.0f);
  return true;
}

float readLux() {
  uint32_t sum = 0;
  for (int i = 0; i < 12; ++i) {
    sum += analogRead(PIN_LIGHT);
    delay(2);
  }
  float raw = (float)sum / 12.0f;
  float volts = (raw / 4095.0f) * 3.3f;
  float lux = volts * 1000.0f;

  if (luxValue == 0.0f) luxValue = lux;
  else luxValue = luxValue * 0.82f + lux * 0.18f;

  return luxValue;
}

void drawDigit(int digit, int x, int y, int s, uint16_t fg, uint16_t bg) {
  static const uint8_t g[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}
  };

  fillRectFast(x, y, 5 * s, 7 * s, bg);
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 5; ++c) {
      if (g[digit][r] & (1 << (4 - c))) {
        fillRectFast(x + c * s, y + r * s, s, s, fg);
      }
    }
  }
}

void drawNumber(int v, int x, int y, int s, uint16_t fg, uint16_t bg) {
  if (v < 0) v = 0;
  if (v > 9999) v = 9999;
  int a = (v / 1000) % 10;
  int b = (v / 100) % 10;
  int c = (v / 10) % 10;
  int d = v % 10;
  int step = s * 6;
  bool started = false;
  int cx = x;

  fillRectFast(x, y, step * 4 + s, 7 * s, bg);
  if (a > 0) {
    drawDigit(a, cx, y, s, fg, bg);
    cx += step;
    started = true;
  }
  if (started || b > 0) {
    drawDigit(b, cx, y, s, fg, bg);
    cx += step;
    started = true;
  }
  if (started || c > 0) {
    drawDigit(c, cx, y, s, fg, bg);
    cx += step;
  }
  drawDigit(d, cx, y, s, fg, bg);
}

void drawFloat1(float v, int x, int y, int s, uint16_t fg, uint16_t bg) {
  if (v < 0) v = 0;
  int n = (int)round(v * 10.0f);
  int whole = n / 10;
  int frac = n % 10;
  int wholeWidth = intWidth(whole, s);
  int dotX = x + wholeWidth + s;
  int fracX = dotX + s * 2;
  int totalWidth = wholeWidth + s * 2 + s * 5;

  fillRectFast(x, y, totalWidth, 7 * s, bg);
  drawNumber(whole, x, y, s, fg, bg);
  fillRectFast(dotX, y + s * 6, s, s, fg);
  drawDigit(frac, fracX, y, s, fg, bg);
}

int digitCount(int v) {
  if (v >= 1000) return 4;
  if (v >= 100) return 3;
  if (v >= 10) return 2;
  return 1;
}

int intWidth(int v, int s) {
  return digitCount(v) * (s * 6) - s;
}

int float1Width(float v, int s) {
  int whole = (int)floor(max(v, 0.0f));
  return intWidth(whole, s) + s * 7;
}

void drawCenteredNumber(int v, int cx, int y, int s, uint16_t fg, uint16_t bg) {
  int width = intWidth(v, s);
  int x = cx - width / 2;
  drawNumber(v, x, y, s, fg, bg);
}

void drawCenteredFloat1(float v, int cx, int y, int s, uint16_t fg, uint16_t bg) {
  int width = float1Width(v, s);
  int x = cx - width / 2;
  drawFloat1(v, x, y, s, fg, bg);
}

void drawLabel(const char* txt, int x, int y, uint16_t fg, uint16_t bg) {
  static const uint8_t map[26][5] = {
    {7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},
    {7,4,5,5,7},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,7},{5,5,6,5,5},{4,4,4,4,7},
    {5,7,7,5,5},{5,7,7,7,5},{7,5,5,5,7},{7,5,7,4,4},{7,5,5,7,1},{7,5,7,6,5},
    {7,4,7,1,7},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
    {5,5,2,2,2},{7,1,2,4,7}
  };

  int cx = x;
  while (*txt) {
    char ch = *txt++;
    fillRectFast(cx, y, 6, 10, bg);
    if (ch >= 'A' && ch <= 'Z') {
      const uint8_t* rows = map[ch - 'A'];
      for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 3; ++c) {
          if (rows[r] & (1 << (2 - c))) {
            fillRectFast(cx + c * 2, y + r * 2, 2, 2, fg);
          }
        }
      }
    }
    cx += 8;
  }
}

void drawSunIcon(int x, int y, uint16_t c, uint16_t bg) {
  fillRectFast(x + 10, y + 10, 12, 12, c);
  fillRectFast(x + 13, y + 2, 6, 5, c);
  fillRectFast(x + 13, y + 25, 6, 5, c);
  fillRectFast(x + 2, y + 13, 5, 6, c);
  fillRectFast(x + 25, y + 13, 5, 6, c);
  fillRectFast(x + 5, y + 5, 4, 4, c);
  fillRectFast(x + 22, y + 5, 4, 4, c);
  fillRectFast(x + 5, y + 22, 4, 4, c);
  fillRectFast(x + 22, y + 22, 4, 4, c);
}

void drawThermoIcon(int x, int y, uint16_t c, uint16_t bg) {
  fillRectFast(x + 13, y + 4, 6, 18, c);
  fillRectFast(x + 9, y + 22, 14, 14, c);
  fillRectFast(x + 12, y + 5, 8, 3, bg);
  fillRectFast(x + 15, y + 8, 2, 13, bg);
}

void drawDropIcon(int x, int y, uint16_t c, uint16_t bg) {
  fillRectFast(x + 13, y + 2, 6, 8, c);
  fillRectFast(x + 9, y + 10, 14, 18, c);
  fillRectFast(x + 6, y + 14, 20, 10, c);
  fillRectFast(x + 12, y + 17, 8, 5, bg);
}

void drawCardShell(int x, int y, int w, int h) {
  uint16_t shadow = c565(167, 214, 255);
  uint16_t card = c565(246, 242, 238);
  uint16_t edge = c565(224, 234, 245);
  fillRectFast(x + 3, y + 4, w, h, shadow);
  fillRectFast(x, y, w, h, card);
  rectFast(x, y, w, h, edge);
}

void drawStaticUi() {
  uint16_t bgTop = c565(183, 229, 255);
  uint16_t bgMid = c565(121, 208, 255);
  uint16_t bgBot = c565(78, 188, 255);
  uint16_t card = c565(244, 241, 237);
  uint16_t edge = c565(164, 221, 255);
  uint16_t divider = c565(222, 228, 235);
  uint16_t luxC = c565(245, 158, 11);
  uint16_t tempC = c565(239, 68, 68);
  uint16_t humC = c565(59, 130, 246);

  fillRectFast(0, 0, W, 86, bgTop);
  fillRectFast(0, 86, W, 104, bgMid);
  fillRectFast(0, 190, W, 130, bgBot);
  fillRectFast(6, 6, 160, 308, c565(226, 244, 255));
  rectFast(6, 6, 160, 308, edge);

  drawCardShell(12, 16, 148, 92);
  drawCardShell(12, 114, 148, 92);
  drawCardShell(12, 212, 148, 92);

  // Fixed icon zone on the left so the icons never overlap the sensor values.
  fillRectFast(VALUE_LEFT - ICON_GAP / 2, 26, 2, 72, divider);
  fillRectFast(VALUE_LEFT - ICON_GAP / 2, 124, 2, 72, divider);
  fillRectFast(VALUE_LEFT - ICON_GAP / 2, 222, 2, 72, divider);

  drawThermoIcon(20, 28, tempC, card);
  drawDropIcon(20, 126, humC, card);
  drawSunIcon(20, 224, luxC, card);
}

void updateDynamicUi() {
  uint16_t card = c565(244, 241, 237);
  uint16_t luxC = c565(245, 158, 11);
  uint16_t tempC = c565(239, 68, 68);
  uint16_t humC = c565(59, 130, 246);
  uint16_t bad = c565(127, 29, 29);

  // Clear only the value zone on the right; the left icon zone stays untouched.
  fillRectFast(VALUE_LEFT, 40, VALUE_RIGHT - VALUE_LEFT, 50, card);
  fillRectFast(VALUE_LEFT, 138, VALUE_RIGHT - VALUE_LEFT, 50, card);
  fillRectFast(VALUE_LEFT, 236, VALUE_RIGHT - VALUE_LEFT, 50, card);

  drawCenteredFloat1(tempValue, VALUE_CENTER_X, 48, 3, tempC, card);

  if (shtOk) {
    drawCenteredFloat1(humValue, VALUE_CENTER_X, 146, 3, humC, card);
    int luxScale = (luxValue >= 1000.0f) ? 3 : 4;
    drawCenteredNumber((int)round(luxValue), VALUE_CENTER_X, luxScale == 4 ? 240 : 246, luxScale, luxC, card);
  } else {
    fillRectFast(VALUE_LEFT, 58, VALUE_RIGHT - VALUE_LEFT, 12, card);
    fillRectFast(VALUE_LEFT, 156, VALUE_RIGHT - VALUE_LEFT, 12, card);
    drawLabel("ERR", VALUE_CENTER_X - 12, 60, bad, card);
    drawLabel("ERR", VALUE_CENTER_X - 12, 158, bad, card);
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LIGHT, ADC_0db);
  Wire.begin(PIN_SDA, PIN_SCL);

  initTft();

  readLux();
  float t = 0.0f;
  float h = 0.0f;
  shtOk = readSht(t, h);
  tempValue = t;
  humValue = h;

  drawStaticUi();
  updateDynamicUi();
}

void loop() {
  unsigned long now = millis();

  if (now - lastLightMs >= 120UL) {
    lastLightMs = now;
    readLux();
  }

  if (now - lastShtMs >= 1000UL) {
    lastShtMs = now;
    float t = 0.0f;
    float h = 0.0f;
    shtOk = readSht(t, h);
    if (shtOk) {
      if (tempValue == 0.0f && humValue == 0.0f) {
        tempValue = t;
        humValue = h;
      } else {
        tempValue = tempValue * 0.80f + t * 0.20f;
        humValue = humValue * 0.80f + h * 0.20f;
      }
    }
  }

  if (now - lastDrawMs >= 200UL) {
    lastDrawMs = now;
    updateDynamicUi();
  }
}
