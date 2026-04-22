#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>

// ----- Wi-Fi settings -----
static constexpr char CAMERA_AP_SSID[] = "XIAO-CAM-HOTSPOT";
static constexpr char CAMERA_AP_PASSWORD[] = "12345678";
static constexpr char CAMERA_HOST[] = "192.168.4.1";

// ----- Display wiring -----
// GND -> GND
// VCC -> 3.3V
// SCL -> GPIO 36
// SDA -> GPIO 35
// RES -> GPIO 6
// DC  -> GPIO 4
// CS  -> GPIO 5
// BLK -> 3.3V
// TRA -> GPIO 2
// TRB -> GPIO 3
// PSH -> GPIO 1
// KO  -> GND
static constexpr int TFT_CS_PIN = 5;
static constexpr int TFT_DC_PIN = 4;
static constexpr int TFT_RST_PIN = 6;
static constexpr int TFT_MOSI_PIN = 35;
static constexpr int TFT_SCLK_PIN = 36;

static constexpr int CAPTURE_BUTTON_PIN = 1;
static constexpr int KNOB_A_PIN = 2;
static constexpr int KNOB_B_PIN = 3;

// Display controller/orientation verified by your test.
static constexpr int DISPLAY_ROTATION = 1;
static constexpr uint16_t DISPLAY_WIDTH = 320;
static constexpr uint16_t DISPLAY_HEIGHT = 240;

// Viewer cadence. Lower interval = smoother live view, higher Wi-Fi/RAM load.
static constexpr uint32_t VIEW_FETCH_INTERVAL_MS = 180;
static constexpr uint32_t STATUS_POLL_INTERVAL_MS = 2000;

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

uint32_t g_lastViewFetchMs = 0;
uint32_t g_lastStatusPollMs = 0;
uint32_t g_capturePauseUntilMs = 0;
bool g_lastButtonState = HIGH;
String g_statusLine = "Booting...";

bool jpgDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (x >= static_cast<int16_t>(tft.width()) || y >= static_cast<int16_t>(tft.height())) {
    return false;
  }

  uint16_t drawW = w;
  uint16_t drawH = h;
  if (x + drawW > tft.width()) {
    drawW = tft.width() - x;
  }
  if (y + drawH > tft.height()) {
    drawH = tft.height() - y;
  }

  tft.drawRGBBitmap(x, y, bitmap, drawW, drawH);
  return true;
}

void drawStatusBar() {
  const int barY = tft.height() - 24;
  tft.fillRect(0, barY, tft.width(), 24, ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, barY + 8);
  tft.print(g_statusLine);
}

void showSplash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(24, 30);
  tft.println("XIAO Camera");
  tft.setCursor(24, 55);
  tft.println("Viewer");
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(24, 90);
  tft.println("Connecting to hotspot...");
}

bool connectToCameraAp() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(CAMERA_AP_SSID, CAMERA_AP_PASSWORD);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 20000) {
    delay(250);
  }

  return WiFi.status() == WL_CONNECTED;
}

bool fetchFrameAndDraw() {
  HTTPClient http;
  String url = String("http://") + CAMERA_HOST + "/jpg_small";
  http.setTimeout(6000);

  Serial.print("Requesting: ");
  Serial.println(url);

  if (!http.begin(url)) {
    Serial.println("HTTP begin failed");
    g_statusLine = "HTTP begin failed";
    return false;
  }

  int httpCode = http.GET();
  Serial.print("HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    g_statusLine = "JPEG fetch failed";
    http.end();
    return false;
  }

  int len = http.getSize();
  Serial.print("JPEG len: ");
  Serial.println(len);

  if (len <= 0 || len > 250000) {
    g_statusLine = "Bad JPEG length";
    http.end();
    return false;
  }

  uint8_t *jpgBuffer = (uint8_t *)ps_malloc(len);
  if (!jpgBuffer) jpgBuffer = (uint8_t *)malloc(len);

  if (!jpgBuffer) {
    Serial.println("Not enough RAM");
    g_statusLine = "Not enough RAM";
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int offset = 0;

  while (http.connected() && offset < len) {
    size_t available = stream->available();
    if (!available) {
      delay(1);
      continue;
    }

    int read = stream->readBytes(jpgBuffer + offset, min((int)available, len - offset));
    if (read <= 0) break;
    offset += read;
  }

  Serial.print("Bytes read: ");
  Serial.println(offset);

  bool success = (offset == len);
  if (success) {
    Serial.println("Drawing JPEG");
    TJpgDec.drawJpg(0, 0, jpgBuffer, len);
    g_statusLine = "Live " + WiFi.localIP().toString();
  } else {
    Serial.println("JPEG read incomplete");
    g_statusLine = "JPEG read incomplete";
  }

  free(jpgBuffer);
  http.end();
  return success;
}


void sendCaptureRequest() {
  g_capturePauseUntilMs = millis() + 2000;
  HTTPClient http;
  String url = String("http://") + CAMERA_HOST + "/capture";
  http.setTimeout(8000);

  if (!http.begin(url)) {
    g_statusLine = "Capture HTTP begin failed";
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    g_statusLine = http.getString();
  } else {
    g_statusLine = "Capture failed";
  }

  http.end();
  drawStatusBar();
}

void pollStatus() {
  HTTPClient http;
  String url = String("http://") + CAMERA_HOST + "/status";
  http.setTimeout(2000);

  if (!http.begin(url)) {
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    if (body.indexOf("\"last_saved\":\"") >= 0) {
      g_statusLine = "Ready to capture";
    }
  }

  http.end();
}

void checkButton() {
  bool pressed = (digitalRead(CAPTURE_BUTTON_PIN) == LOW);
  bool wasPressed = (g_lastButtonState == LOW);

  if (pressed && !wasPressed) {
    sendCaptureRequest();
  }

  g_lastButtonState = pressed ? LOW : HIGH;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("Display board booted");


  pinMode(CAPTURE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(KNOB_A_PIN, INPUT_PULLUP);
  pinMode(KNOB_B_PIN, INPUT_PULLUP);

  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  SPI.setFrequency(10000000);

  tft.init(240, 320);
  tft.setRotation(DISPLAY_ROTATION);
  showSplash();

  TJpgDec.setCallback(jpgDrawCallback);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);

  if (!connectToCameraAp()) {
    g_statusLine = "Wi-Fi connect failed";
    drawStatusBar();
    return;
  }

  tft.fillScreen(ST77XX_BLACK);
  g_statusLine = "Connected " + WiFi.localIP().toString();
  drawStatusBar();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (connectToCameraAp()) {
      g_statusLine = "Reconnected";
    } else {
      g_statusLine = "Wi-Fi lost";
      drawStatusBar();
      delay(500);
      return;
    }
  }

  uint32_t now = millis();

  checkButton();

  if (now - g_lastViewFetchMs >= VIEW_FETCH_INTERVAL_MS) {
    if (millis() >= g_capturePauseUntilMs) {
      g_lastViewFetchMs = now;
      fetchFrameAndDraw();
      drawStatusBar();
    }
  }

  if (now - g_lastStatusPollMs >= STATUS_POLL_INTERVAL_MS) {
    g_lastStatusPollMs = now;
    pollStatus();
  }

  delay(5);
}
