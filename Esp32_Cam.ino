#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// Camera pins for AI-THINKER model
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define LED_PIN 4      // Built-in flash LED
#define LOCK_PIN 2     // Solenoid lock control pin

// WiFi credentials
const char *ssid = "wifi name";
const char *password = "wifi password";

httpd_handle_t camera_httpd = NULL;

// Access control variables
unsigned long accessGrantedTime = 0;
bool accessGranted = false;

// LED control handlers
static esp_err_t led_on_handler(httpd_req_t *req) {
  digitalWrite(LED_PIN, HIGH);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "LED ON", 6);
}

static esp_err_t led_off_handler(httpd_req_t *req) {
  digitalWrite(LED_PIN, LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "LED OFF", 7);
}

// Grant access handler - called by Python when face is recognized
static esp_err_t grant_access_handler(httpd_req_t *req) {
  Serial.println("=== ACCESS GRANTED ===");
  accessGranted = true;
  accessGrantedTime = millis();
  
  // Unlock solenoid
  digitalWrite(LOCK_PIN, HIGH);
  Serial.println("Door unlocked");
  
  // Turn on LED solid
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON - Starting 10 second timer");
  
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "ACCESS GRANTED - 10 seconds", 27);
}

// Capture handler
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// Stream handler
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[64];

  static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
  static const char *_STREAM_BOUNDARY = "\r\n--frame\r\n";
  static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
      }
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) {
      break;
    }
  }

  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  httpd_uri_t led_on_uri = {
    .uri = "/led/on",
    .method = HTTP_GET,
    .handler = led_on_handler,
    .user_ctx = NULL
  };

  httpd_uri_t led_off_uri = {
    .uri = "/led/off",
    .method = HTTP_GET,
    .handler = led_off_handler,
    .user_ctx = NULL
  };

  httpd_uri_t grant_access_uri = {
    .uri = "/grant",
    .method = HTTP_GET,
    .handler = grant_access_handler,
    .user_ctx = NULL
  };

  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &led_on_uri);
    httpd_register_uri_handler(camera_httpd, &led_off_uri);
    httpd_register_uri_handler(camera_httpd, &grant_access_uri);
    Serial.println("All HTTP handlers registered successfully");
  }
}

void handleAccessControl() {
  if (!accessGranted) {
    return; // No active access period
  }

  unsigned long elapsed = millis() - accessGrantedTime;

  // 0-7 seconds: LED solid ON, access granted
  if (elapsed < 7000) {
    // Keep LED solid (already on)
    digitalWrite(LED_PIN, HIGH);
  }
  // 7-10 seconds: LED blinking, warning period
  else if (elapsed < 10000) {
    // Blink LED fast (200ms on, 200ms off)
    unsigned long blinkCycle = millis() / 200;
    digitalWrite(LED_PIN, blinkCycle % 2);
  }
  // After 10 seconds: Lock and reset
  else {
    Serial.println("=== ACCESS PERIOD ENDED ===");
    
    // Turn off LED
    digitalWrite(LED_PIN, LOW);
    
    // Capture image to check for face (optional)
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      Serial.println("Final image captured for face check");
      esp_camera_fb_return(fb);
    }
    
    // Lock the solenoid
    digitalWrite(LOCK_PIN, LOW);
    Serial.println("Door locked");
    
    // Reset access control
    accessGranted = false;
    accessGrantedTime = 0;
    Serial.println("Ready for next access");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // LED and Lock Setup
  pinMode(LED_PIN, OUTPUT);
  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LOCK_PIN, LOW);  // Start locked
  
  Serial.println("GPIO pins initialized - Door LOCKED");

  // Camera configuration
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
  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);
  Serial.println("Camera initialized successfully");

  // WiFi connection
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.println("\n========================================");
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  Serial.println("========================================");
  Serial.println("Available endpoints:");
  Serial.println("  /capture - Get single image");
  Serial.println("  /stream - Video stream");
  Serial.println("  /led/on - Turn LED on manually");
  Serial.println("  /led/off - Turn LED off manually");
  Serial.println("  /grant - Grant access (10 sec timer)");
  Serial.println("========================================");
  Serial.println("System ready - Waiting for face recognition...\n");
}

void loop() {
  handleAccessControl();
  delay(50);  // Check every 50ms for responsive LED blinking
}