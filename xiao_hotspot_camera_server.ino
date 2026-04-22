#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <esp_http_server.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>

// XIAO ESP32S3 Sense camera pin map from Seeed's camera examples/wiki.
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39

#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

static constexpr int SD_CS_PIN = 21;
static constexpr char AP_SSID[] = "XIAO-CAM-HOTSPOT";
static constexpr char AP_PASSWORD[] = "12345678";
static constexpr char HOST_NAME[] = "xiao-cam";

// OV2640 max practical resolution is UXGA (1600x1200).
// If you use an OV5640 and want to experiment with a bigger frame, replace
// FRAMESIZE_UXGA with FRAMESIZE_QSXGA after confirming your ESP32 core supports it.
static framesize_t g_streamFrameSize = FRAMESIZE_UXGA;
static int g_jpegQuality = 10;  // Lower number = better JPEG quality.
static framesize_t g_displayFrameSize = FRAMESIZE_QVGA;
static int g_displayJpegQuality = 14;

SemaphoreHandle_t g_cameraMutex;
httpd_handle_t g_cameraHttpd = nullptr;
String g_lastSavedPath = "";
volatile uint32_t g_captureCounter = 0;
volatile bool g_pauseStreaming = false;

static const char PROGMEM INDEX_HTML[] = R"HTML(
<!doctype html>
<html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>XIAO ESP32S3 Sense Camera</title>
    <style>
      body { font-family: Arial, sans-serif; background: #111; color: #eee; margin: 0; padding: 16px; }
      .wrap { max-width: 1200px; margin: 0 auto; }
      img { width: 100%; height: auto; border-radius: 12px; background: #222; }
      button { padding: 12px 18px; border: 0; border-radius: 10px; font-size: 16px; cursor: pointer; }
      .row { display: flex; gap: 12px; align-items: center; margin: 16px 0; flex-wrap: wrap; }
      .status { color: #8fd18f; }
      code { background: #222; padding: 2px 6px; border-radius: 6px; }
    </style>
  </head>
  <body>
    <div class="wrap">
      <h1>XIAO ESP32S3 Sense Live Stream</h1>
      <div class="row">
        <button onclick="capture()">Capture To SD Card</button>
        <span class="status" id="status">Ready</span>
      </div>
      <p>Open this page from any device connected to the XIAO hotspot. Raw stream URL: <code>/stream</code></p>
      <img src="/stream" alt="Live stream" />
    </div>
    <script>
      async function capture() {
        const status = document.getElementById('status');
        status.textContent = 'Capturing...';
        try {
          const response = await fetch('/capture');
          const text = await response.text();
          status.textContent = text;
        } catch (error) {
          status.textContent = 'Capture failed';
        }
      }
    </script>
  </body>
</html>
)HTML";

String makeCapturePath() {
  uint32_t id = ++g_captureCounter;
  char path[40];
  snprintf(path, sizeof(path), "/capture_%06lu.jpg", static_cast<unsigned long>(id));
  return String(path);
}

bool saveFrameToSd(camera_fb_t *fb, String &savedPath) {
  savedPath = makeCapturePath();
  File file = SD.open(savedPath.c_str(), FILE_WRITE);
  if (!file) {
    return false;
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();
  if (written != fb->len) {
    SD.remove(savedPath.c_str());
    return false;
  }

  g_lastSavedPath = savedPath;
  return true;
}

esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, reinterpret_cast<const char *>(INDEX_HTML), HTTPD_RESP_USE_STRLEN);
}

esp_err_t jpgHandler(httpd_req_t *req) {
  if (xSemaphoreTake(g_cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(g_cameraMutex);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);

  esp_camera_fb_return(fb);
  xSemaphoreGive(g_cameraMutex);
  return res;
}

esp_err_t jpgSmallHandler(httpd_req_t *req) {
  if (g_pauseStreaming) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture in progress");

    return ESP_FAIL;
  }

  if (xSemaphoreTake(g_cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    xSemaphoreGive(g_cameraMutex);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  sensor->set_framesize(sensor, g_displayFrameSize);
  sensor->set_quality(sensor, g_displayJpegQuality);
  delay(80);

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    sensor->set_framesize(sensor, g_streamFrameSize);
    sensor->set_quality(sensor, g_jpegQuality);
    xSemaphoreGive(g_cameraMutex);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);

  esp_camera_fb_return(fb);

  sensor->set_framesize(sensor, g_streamFrameSize);
  sensor->set_quality(sensor, g_jpegQuality);
  xSemaphoreGive(g_cameraMutex);
  return res;
}

esp_err_t captureHandler(httpd_req_t *req) {
  g_pauseStreaming = true;
  delay(120);

  if (xSemaphoreTake(g_cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    g_pauseStreaming = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
    return ESP_FAIL;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(g_cameraMutex);
    g_pauseStreaming = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
    return ESP_FAIL;
  }

  String savedPath;
  bool ok = saveFrameToSd(fb, savedPath);

  esp_camera_fb_return(fb);
  xSemaphoreGive(g_cameraMutex);
  g_pauseStreaming = false;

  if (!ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD save failed");
    return ESP_FAIL;
  }

  String message = "Saved " + savedPath;
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, message.c_str(), message.length());
}

esp_err_t statusHandler(httpd_req_t *req) {
  String json = "{";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"last_saved\":\"" + g_lastSavedPath + "\",";
  json += "\"captures\":" + String(g_captureCounter);
  json += "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

esp_err_t streamHandler(httpd_req_t *req) {
  static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
  static const char *BOUNDARY = "\r\n--frame\r\n";
  static const char *PART_HEADER = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");

  while (true) {
    if (g_pauseStreaming) {
      delay(30);
      continue;
    }

    if (xSemaphoreTake(g_cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
      return ESP_FAIL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(g_cameraMutex);
      return ESP_FAIL;
    }

    char header[64];
    size_t headerLen = snprintf(header, sizeof(header), PART_HEADER, fb->len);

    esp_err_t res = httpd_resp_send_chunk(req, BOUNDARY, strlen(BOUNDARY));
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, header, headerLen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
    }

    esp_camera_fb_return(fb);
    xSemaphoreGive(g_cameraMutex);

    if (res != ESP_OK) {
      break;
    }

    delay(10);
  }

  return ESP_OK;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;

  if (httpd_start(&g_cameraHttpd, &config) != ESP_OK) {
    Serial.println("Failed to start camera web server");
    return;
  }

  httpd_uri_t indexUri = { .uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr };
  httpd_uri_t jpgUri = { .uri = "/jpg", .method = HTTP_GET, .handler = jpgHandler, .user_ctx = nullptr };
  httpd_uri_t jpgSmallUri = { .uri = "/jpg_small", .method = HTTP_GET, .handler = jpgSmallHandler, .user_ctx = nullptr };
  httpd_uri_t streamUri = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr };
  httpd_uri_t captureUri = { .uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr };
  httpd_uri_t statusUri = { .uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr };

  httpd_register_uri_handler(g_cameraHttpd, &indexUri);
  httpd_register_uri_handler(g_cameraHttpd, &jpgUri);
  httpd_register_uri_handler(g_cameraHttpd, &jpgSmallUri);
  httpd_register_uri_handler(g_cameraHttpd, &streamUri);
  httpd_register_uri_handler(g_cameraHttpd, &captureUri);
  httpd_register_uri_handler(g_cameraHttpd, &statusUri);
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = g_streamFrameSize;
  config.jpeg_quality = g_jpegQuality;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);
    sensor->set_quality(sensor, g_jpegQuality);
    sensor->set_framesize(sensor, g_streamFrameSize);
  }

  return true;
}

bool initSdCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD.begin() failed");
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card detected");
    return false;
  }

  uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("SD card ready: %llu MB\n", cardSizeMB);
  return true;
}

void startHotspot() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, 1, 0, 4);
  if (!ok) {
    Serial.println("softAP start failed");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  Serial.print("Hotspot SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Hotspot password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("Open http://");
  Serial.println(ip);

  if (MDNS.begin(HOST_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS name: http://%s.local\n", HOST_NAME);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  g_cameraMutex = xSemaphoreCreateMutex();
  if (!g_cameraMutex) {
    Serial.println("Failed to create camera mutex");
    while (true) {
      delay(1000);
    }
  }

  if (!initCamera()) {
    while (true) {
      delay(1000);
    }
  }

  if (!initSdCard()) {
    while (true) {
      delay(1000);
    }
  }

  startHotspot();
  startCameraServer();

  Serial.println("Camera server ready.");
  Serial.println("Endpoints:");
  Serial.println("  /        browser UI");
  Serial.println("  /stream  MJPEG stream");
  Serial.println("  /jpg     single JPEG frame");
  Serial.println("  /jpg_small low-res JPEG for display client");
  Serial.println("  /capture save JPEG to SD");
  Serial.println("  /status  JSON status");
}

void loop() {
  delay(1000);
}
