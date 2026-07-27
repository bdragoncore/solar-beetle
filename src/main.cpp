#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <mbedtls/sha256.h>
#include <mbedtls/rsa.h>
#include <mbedtls/pk.h>
#include "public_key.h"
#include "credentials.h"
#include <time.h>
#include <esp_bt.h>

#ifndef WIFI_SSID
#error "WIFI_SSID not defined. Create .env from .env.sample"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD not defined. Create .env from .env.sample"
#endif
#ifndef OTA_PASSWORD
#error "OTA_PASSWORD not defined. Create .env from .env.sample"
#endif

#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "solar"
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "solar"
#endif
#ifndef LAT
#error "LAT not defined. Add LAT and LON to .env"
#endif
#ifndef LON
#error "LON not defined. Add LAT and LON to .env"
#endif
#ifndef TZ
#define TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef LIGHTS_OFF_TIME
#define LIGHTS_OFF_TIME "1.0"
#endif

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

WebServer server(80);

const int ADC_PIN = 34;
const float DIVIDER_RATIO = 2.0;
const float VREF = 3.3;

const int RGB_PIN = 5;
const int BLUE_LED_PIN = 2;
const int NUM_PIXELS = 1;
Adafruit_NeoPixel pixel(NUM_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

const int LIGHT_PIN = 17;
enum LightMode { LIGHT_ON, LIGHT_OFF, LIGHT_AUTO };
LightMode lightMode = LIGHT_AUTO;
bool lightState = false;
double sunriseTime = 6.0;
double sunsetTime = 18.0;
double lightsOffTime = atof(LIGHTS_OFF_TIME);
unsigned long lastSunCalc = 0;

const float LOW_BAT_THRESHOLD = 3.40f;
const float CRITICAL_BAT_THRESHOLD = 3.30f;
bool powerSaving = false;
bool cloudy = false;
float cloudVoltageRef = -1.0f;
int lastCloudCheckDay = -1;
unsigned long lastVoltTrendCheck = 0;

bool isCharging = false;
float chargeRefVoltage = -1.0f;
unsigned long lastChargeCheck = 0;
float voltageRate = 0.0f;
const unsigned long CHARGE_INTERVAL = 300000;
const float CHARGE_RISE_THRESH = 0.03f;
const float CHARGE_DROP_THRESH = -0.01f;

enum PowerMode { POWER_HIGH, POWER_LOW };
PowerMode currentPowerMode = POWER_HIGH;
unsigned long lastPowerModeCheck = 0;

void applyPowerMode(float v) {
  unsigned long now = millis();
  if (now - lastPowerModeCheck < 10000) return;
  lastPowerModeCheck = now;
  PowerMode target = v >= 3.50f ? POWER_HIGH : (v < LOW_BAT_THRESHOLD ? POWER_LOW : currentPowerMode);
  if (target == currentPowerMode) return;
  currentPowerMode = target;
  if (target == POWER_LOW) {
    setCpuFrequencyMhz(160);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  } else {
    setCpuFrequencyMhz(240);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
  }
}

float batteryVoltage() {
  analogSetPinAttenuation(ADC_PIN, ADC_11db);
  uint32_t millivolts = analogReadMilliVolts(ADC_PIN);
  return (millivolts * DIVIDER_RATIO) / 1000.0;
}

int batteryPercent(float volts) {
  if (volts > 4.05f) return 100;
  if (volts <= 3.20) return 0;
  return (int)((volts - 3.20) * 100.0 / (4.20 - 3.20));
}

int dayOfYear(int y, int m, int d) {
  int n1 = 275 * m / 9;
  int n2 = (m + 9) / 12;
  int n3 = 1 + (y - 4 * (y / 4) + 2) / 3;
  return n1 - n2 * n3 + d - 30;
}

void calcSunriseSunset(int doy, double lat, double lon, double tz, double &rise, double &set) {
  double g = radians(357.5291 + 0.98560028 * doy);
  double c = 1.9148 * sin(g) + 0.0200 * sin(2 * g) + 0.0003 * sin(3 * g);
  double l = radians(280.4665 + 0.98564736 * doy + c);
  double e = radians(23.4393 - 0.00000036 * doy);
  double y = pow(tan(e / 2.0), 2);
  double eot = (y * sin(2 * l) + 4 * y * sin(g) * cos(2 * l) - 0.5 * y * y * sin(4 * l)) * 4.0;
  double decl = degrees(asin(sin(e) * sin(l)));
  double ha = degrees(acos(-tan(radians(lat)) * tan(radians(decl))));
  double noon = 720 - 4 * lon - eot;
  rise = (noon - 4 * ha) / 60.0 + tz;
  set = (noon + 4 * ha) / 60.0 + tz;
}

void detectCharging() {
  unsigned long now = millis();
  unsigned long dt = now - lastChargeCheck;
  if (dt < CHARGE_INTERVAL) return;
  float v = batteryVoltage();
  if (chargeRefVoltage > 0) {
    float change = v - chargeRefVoltage;
    voltageRate = change / (dt / 3600000.0f);
    if (change > CHARGE_RISE_THRESH)
      isCharging = true;
    else if (change < CHARGE_DROP_THRESH)
      isCharging = false;
  }
  lastChargeCheck = now;
  chargeRefVoltage = v;
}

void updateLight() {
  float v = batteryVoltage();
  applyPowerMode(v);

  if (v < CRITICAL_BAT_THRESHOLD) {
    digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = true;
    return;
  }

  if (lightMode == LIGHT_ON) { digitalWrite(LIGHT_PIN, HIGH); lightState = true; powerSaving = false; return; }
  if (lightMode == LIGHT_OFF) { digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = false; return; }

  time_t now = time(nullptr);
  if (now < 100000) { digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = false; return; }
  struct tm *tm = localtime(&now);
  int doy = dayOfYear(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
  if (now - lastSunCalc > 43200) {
    double tz = tm->tm_isdst > 0 ? -4.0 : -5.0;
    calcSunriseSunset(doy, atof(LAT), atof(LON), tz, sunriseTime, sunsetTime);
    lastSunCalc = now;
  }

  double cur = tm->tm_hour + tm->tm_min / 60.0 + tm->tm_sec / 3600.0;
  bool isNight = (cur >= sunsetTime || cur < sunriseTime);

  if (doy != lastCloudCheckDay) {
    lastCloudCheckDay = doy;
    cloudVoltageRef = -1.0f;
    if (!isNight) cloudVoltageRef = v;
    cloudy = false;
  }

  if (!isNight) {
    if (cloudVoltageRef < 0.0f) cloudVoltageRef = v;
    if (now - lastVoltTrendCheck > 600000) {
      cloudy = cloudVoltageRef < 4.05f && (v - cloudVoltageRef) < 0.05f;
      lastVoltTrendCheck = now;
    }
    digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = false;
    return;
  }

  if (isNight && cur >= lightsOffTime && cur < sunriseTime) {
    digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = false;
    return;
  }

  if (v < LOW_BAT_THRESHOLD || (cloudy && v < 4.05f)) {
    digitalWrite(LIGHT_PIN, LOW); lightState = false; powerSaving = true;
    return;
  }

  digitalWrite(LIGHT_PIN, HIGH); lightState = true; powerSaving = false;
}

String pieSector(double h1, double h2, double r, const char *color) {
  if (h2 - h1 < 0.001) return "";
  double a1 = (h1 / 24.0) * 2.0 * PI - PI / 2.0;
  double a2 = (h2 / 24.0) * 2.0 * PI - PI / 2.0;
  double x1 = 50.0 + r * cos(a1), y1 = 50.0 + r * sin(a1);
  double x2 = 50.0 + r * cos(a2), y2 = 50.0 + r * sin(a2);
  int large = ((h2 - h1) / 24.0 * 360.0) > 180 ? 1 : 0;
  return "<path d=\"M 50 50 L " + String(x1, 1) + " " + String(y1, 1)
    + " A " + String(r, 1) + " " + String(r, 1) + " 0 " + large + " 1 "
    + String(x2, 1) + " " + String(y2, 1)
    + " Z\" fill=\"" + color + "\" stroke=\"none\"/>";
}

String lightDialSVG() {
  time_t now = time(nullptr);
  if (now < 100000) return "";
  struct tm *tm = localtime(&now);
  double cur = tm->tm_hour + tm->tm_min / 60.0 + tm->tm_sec / 3600.0;
  const double r = 42;
  const char *onColor = "#4ade80";
  const char *offColor = "#374151";

  String s;
  s += "<circle cx=\"50\" cy=\"50\" r=\"" + String(r, 1) + "\" fill=\"" + offColor + "\" stroke=\"#4b5563\" stroke-width=\"1.5\"/>";
  s += "<g transform=\"rotate(" + String(-(cur / 24.0) * 360.0, 1) + " 50 50)\">";

  double onStart = sunsetTime;
  double onEnd = lightsOffTime;
  if (onEnd > onStart) {
    s += pieSector(onStart, onEnd, r, onColor);
  } else {
    s += pieSector(onStart, 24.0, r, onColor);
    s += pieSector(0.0, onEnd, r, onColor);
  }

  s += "</g>";
  s += "<polygon points=\"50,4 46,13 54,13\" fill=\"white\"/>";

  return "<svg viewBox=\"0 0 100 100\" width=\"100\" height=\"100\" style=\"margin:4px auto;display:block\">" + s + "</svg>";
}

String lightHtml() {
  String modeStr = lightMode == LIGHT_AUTO ? "auto" : (lightMode == LIGHT_ON ? "on" : "off");
  String stateLabel = lightState ? "On" : "Off";
  String sunriseStr = String((int)sunriseTime) + ":" + (sunriseTime - (int)sunriseTime < 0.5 ? "00" : "30");
  String sunsetStr = String((int)sunsetTime) + ":" + (sunsetTime - (int)sunsetTime < 0.5 ? "00" : "30");
  String note = "";
  if (powerSaving && lightMode == LIGHT_AUTO) {
    note = "<span class=\"sub\" style=\"color:#f87171;font-size:0.65rem\">"
      + String(cloudy ? "Cloudy — conserving" : "Battery low — conserving") + "</span>";
  }
  String dial = "";
  String schedule = "";
  if (lightMode == LIGHT_AUTO) {
    dial = lightDialSVG();
    schedule = "<span class=\"sub\" style=\"font-size:0.65rem\">"
      + sunsetStr + " — " + String((int)lightsOffTime) + ":00"
      + "</span>";
  }
  return "<div class=\"metric\" id=\"light-control\">"
    "<span class=\"label\">String Lights</span>"
    "<span class=\"value\">" + stateLabel + "</span>"
    + schedule + dial +
    "<div class=\"sub\">"
    "<button class=\"btn-sm" + String(lightMode == LIGHT_ON ? " active" : "") + "\" hx-post=\"/api/light\" hx-vals='{\"mode\":\"on\"}' hx-target=\"#light-control\" hx-swap=\"outerHTML\">On</button> "
    "<button class=\"btn-sm" + String(lightMode == LIGHT_OFF ? " active" : "") + "\" hx-post=\"/api/light\" hx-vals='{\"mode\":\"off\"}' hx-target=\"#light-control\" hx-swap=\"outerHTML\">Off</button> "
    "<button class=\"btn-sm" + String(lightMode == LIGHT_AUTO ? " active" : "") + "\" hx-post=\"/api/light\" hx-vals='{\"mode\":\"auto\"}' hx-target=\"#light-control\" hx-swap=\"outerHTML\">Auto</button>"
    "</div>" + note + "</div>";
}

void handleLightGet() {
  server.send(200, "text/html", lightHtml());
}

void handleLightPost() {
  String mode = server.arg("mode");
  if (mode == "on") lightMode = LIGHT_ON;
  else if (mode == "off") lightMode = LIGHT_OFF;
  else if (mode == "auto") lightMode = LIGHT_AUTO;
  updateLight();
  handleLightGet();
}

String batteryHtml() {
  float v = batteryVoltage();
  int pct = batteryPercent(v);
  String color = v < 3.40 ? "#f87171" : (v < 3.70 ? "#fbbf24" : (v > 4.05f ? "#4ade80" : "#fbbf24"));
  bool charged = v > 4.05f && !isCharging;
  String chargeIcon;
  String chargeLabel;
  String fillAnim;
  if (isCharging) {
    chargeIcon = "<svg class=\"bolt\" viewBox=\"0 0 12 20\" width=\"16\" height=\"26\"><path d=\"M7 0L0 11h4.5L3 20l8-11H6.5L9 0z\" fill=\"#4ade80\"/></svg>";
    chargeLabel = "Charging";
    fillAnim = "<animate attributeName=\"opacity\" values=\"0.4;1;0.4\" dur=\"0.8s\" repeatCount=\"indefinite\"/>";
  } else if (charged) {
    chargeIcon = "<svg class=\"bolt\" viewBox=\"0 0 12 20\" width=\"16\" height=\"26\"><path d=\"M7 0L0 11h4.5L3 20l8-11H6.5L9 0z\" fill=\"#4ade80\"/></svg>";
    chargeLabel = "Charged";
    fillAnim = "<animate attributeName=\"opacity\" values=\"0.7;1;0.7\" dur=\"2s\" repeatCount=\"indefinite\"/>";
  } else {
    chargeLabel = v >= 3.70 ? "Nominal" : (v >= 3.30 ? "Low" : "Critical");
    fillAnim = "<animate attributeName=\"opacity\" values=\"0.7;1;0.7\" dur=\"2s\" repeatCount=\"indefinite\"/>";
  }

  int fill = (32 * pct) / 100;
  return "<div class=\"metric battery-card\">"
    "<span class=\"label\">" + chargeIcon + "Battery</span>"
    "<svg class=\"battery-icon\" viewBox=\"0 0 44 24\" width=\"88\" height=\"48\">"
    "<rect x=\"2\" y=\"3\" width=\"36\" height=\"18\" rx=\"3\" fill=\"none\" stroke=\"" + color + "\" stroke-width=\"1.5\"/>"
    "<rect x=\"38\" y=\"8\" width=\"4\" height=\"8\" rx=\"1\" fill=\"" + color + "\"/>"
    "<rect x=\"6\" y=\"6\" width=\"" + String(fill) + "\" height=\"12\" rx=\"1.5\" fill=\"" + color + "\">"
    + fillAnim +
    "</rect>"
    "</svg>"
    "<span class=\"value\">" + String(v, 2) + " V</span>"
    "<span class=\"sub\">" + String(pct) + "% (" + chargeLabel + ")</span>"
    "<span class=\"sub\" style=\"color:#4ade80;font-size:0.6rem\">" + String(isCharging ? "⚡ USB" : (v > 4.05f ? "USB connected" : "")) + " " + String(voltageRate > 0 ? "+" : "") + String(voltageRate, 2) + " V/h</span>"
    "</div>";
}

String systemHtml() {
  unsigned long up = millis() / 1000;
  int days = up / 86400; up %= 86400;
  int hrs = up / 3600; up %= 3600;
  int mins = up / 60; up %= 60;
  String uptime = String(days) + "d " + String(hrs) + "h " + String(mins) + "m " + String(up) + "s";

  return "<div class=\"metric\">"
    "<span class=\"label\">System</span>"
    "<span class=\"value\">" + String(ESP.getCpuFreqMHz()) + " MHz</span>"
    "<span class=\"sub\">Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB</span>"
    "</div>"
    "<div class=\"metric\">"
    "<span class=\"label\">Temperature</span>"
    "<span class=\"value\">" + String(temperatureRead(), 1) + " °C</span>"
    "<span class=\"sub\">" + String(temperatureRead() * 1.8 + 32, 1) + " °F</span>"
    "</div>"
    "<div class=\"metric\">"
    "<span class=\"label\">Wi-Fi</span>"
    "<span class=\"value\">" + String(WiFi.RSSI()) + " dBm</span>"
    "<span class=\"sub\">" + WiFi.localIP().toString() + "</span>"
    "</div>"
    "<div class=\"metric\">"
    "<span class=\"label\">Uptime</span>"
    "<span class=\"value\">" + uptime + "</span>"
    "<span class=\"sub\" style=\"font-size:0.6rem;color:#555\">" + ESP.getSketchMD5().substring(0, 12) + "</span>"
    "</div>";
}

String firmwareHtml() {
  String fwHash = ESP.getSketchMD5();
  return "<div class=\"metric\">"
    "<span class=\"label\">Firmware</span>"
    "<span class=\"value\" style=\"font-size:0.7rem;word-break:break-all;font-family:monospace\">" + fwHash + "</span>"
    "</div>";
}

void handleStatus() {
  String html = batteryHtml() + lightHtml() + systemHtml() + firmwareHtml();
  server.send(200, "text/html", html);
}

void handleStatusJson() {
  float v = batteryVoltage();
  String modeStr = lightMode == LIGHT_AUTO ? "auto" : (lightMode == LIGHT_ON ? "on" : "off");
  String json = "{\"battery\":{\"voltage\":" + String(v, 2) +
                ",\"percent\":" + String(batteryPercent(v)) +
                ",\"charged\":" + String(v > 4.05f ? "true" : "false") +
                ",\"charging\":" + String(isCharging ? "true" : "false") +
                ",\"rate\":" + String(voltageRate, 2) +
                "},\"light\":{\"mode\":\"" + modeStr + "\",\"state\":" + String(lightState ? "true" : "false") + ",\"powerSaving\":" + String(powerSaving ? "true" : "false") + ",\"cloudy\":" + String(cloudy ? "true" : "false") + "}" +
                ",\"system\":{\"cpu\":" + String(ESP.getCpuFreqMHz()) +
                ",\"freeHeap\":" + String(ESP.getFreeHeap()) +
                ",\"temp\":" + String(temperatureRead(), 1) +
                ",\"rssi\":" + String(WiFi.RSSI()) +
                ",\"uptime\":" + String(millis() / 1000) +
                "},\"firmware\":{\"hash\":\"" + ESP.getSketchMD5() + "\"}}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  File file = SPIFFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain", "SPIFFS error");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

bool verifySignature(const uint8_t* hash, const uint8_t* sig, size_t sigLen) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);

  if (mbedtls_pk_parse_public_key(&pk, RSA_PUBLIC_KEY, RSA_PUBLIC_KEY_LEN) != 0) {
    mbedtls_pk_free(&pk);
    return false;
  }

  bool ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 0, sig, sigLen) == 0;
  mbedtls_pk_free(&pk);
  return ok;
}

void handleUpdateGet() {
  String html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
    "<title>Solar Beetle — Update</title>"
    "<style>body{font-family:system-ui;background:#1a1a2e;color:#eee;max-width:500px;margin:2rem auto;padding:0 1rem}"
    "h1{font-size:1.5rem}a{color:#4ade80}.card{background:#16213e;border-radius:10px;padding:1.5rem;margin:1rem 0}"
    "input[type=file]{display:block;margin:1rem 0}button{background:#4ade80;color:#000;border:none;padding:0.5rem 1.5rem;border-radius:6px;font-weight:600}"
    ".note{color:#fbbf24;font-size:0.8rem;margin-top:0.5rem}</style>"
    "</head><body><h1>Firmware Update</h1><a href=\"/\">&larr; Dashboard</a>"
    "<div class=\"card\"><form method=\"POST\" enctype=\"multipart/form-data\">"
    "<label>Select signed firmware .signed.bin file:</label>"
    "<input type=\"file\" name=\"firmware\" accept=\".signed.bin,.bin\" required>"
    "<button type=\"submit\">Upload &amp; Flash</button>"
    "<div class=\"note\">Only RSA-2048 SHA-256 signed firmware accepted.<br>"
    "Use <code>tools/sign_firmware.sh</code> to sign a build.</div>"
    "</form></div></body></html>";
  server.send(200, "text/html", html);
}

mbedtls_sha256_context sha256Ctx;
uint8_t uploadSig[256];
size_t uploadSigLen;

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadSigLen = 0;
    size_t fwSize = upload.totalSize > 256 ? upload.totalSize - 256 : 0;
    if (!Update.begin(fwSize)) {
      Update.printError(Serial);
    }
    mbedtls_sha256_init(&sha256Ctx);
    mbedtls_sha256_starts(&sha256Ctx, 0);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    size_t fwRemaining = Update.size() - Update.progress();
    size_t fwPart = upload.currentSize;
    if (fwPart > fwRemaining) fwPart = fwRemaining;

    if (fwPart > 0) {
      mbedtls_sha256_update(&sha256Ctx, upload.buf, fwPart);
      Update.write(upload.buf, fwPart);
    }

    size_t sigPart = upload.currentSize - fwPart;
    if (sigPart > 0) {
      memcpy(uploadSig + uploadSigLen, upload.buf + fwPart, sigPart);
      uploadSigLen += sigPart;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha256Ctx, hash);
    mbedtls_sha256_free(&sha256Ctx);

    if (uploadSigLen != 256) {
      Serial.printf("Bad sig length: %u\n", uploadSigLen);
      return;
    }

    if (verifySignature(hash, uploadSig, uploadSigLen)) {
      Serial.println("RSA signature VERIFIED");
      Update.end(true);
    } else {
      Serial.println("SIGNATURE MISMATCH");
    }
  }
}

void handleUpdatePost() {
  if (Update.hasError()) {
    server.send(500, "text/html",
      "<html><body style='background:#1a1a2e;color:#f87171;font-family:system-ui;text-align:center;padding:4rem'>"
      "<h1>Update Failed</h1><p>Signature invalid or flash error</p>"
      "<a href=\"/update\" style='color:#4ade80'>Try again</a></body></html>");
  } else {
    server.send(200, "text/html",
      "<html><body style='background:#1a1a2e;color:#4ade80;font-family:system-ui;text-align:center;padding:4rem'>"
      "<h1>Update Complete!</h1><p>Signature OK — rebooting...</p></body></html>");
    delay(100);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_ota_mark_app_valid_cancel_rollback();

  btStop();
  esp_bt_controller_deinit();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
  Serial.println("Bluetooth disabled");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  configTzTime(TZ, "pool.ntp.org");
  Serial.println("NTP sync started");

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("mDNS: http://solar.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("mDNS failed");
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA end"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("OTA: %u%%\r", p / (t / 100));
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error: %u\n", e);
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  server.on("/api/light", HTTP_GET, handleLightGet);
  server.on("/api/light", HTTP_POST, handleLightPost);
  server.begin();
  Serial.println("HTTP server started");

  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, LOW);

  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);

  pixel.begin();
  pixel.setBrightness(30);
  pixel.show();
}

unsigned long lastBlueRead = 0;

void updateLeds() {
  unsigned long now = millis();

  // Blue LED: solid on when USB/charged (≥4.10V)
  if (now - lastBlueRead >= 500) {
    lastBlueRead = now;
    digitalWrite(BLUE_LED_PIN, batteryVoltage() > 4.05f ? HIGH : LOW);
  }

  // RGB WS2812: purple flash every 3s when WiFi connected + server active
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long cycle = now % 3000;
    if (cycle < 80) {
      pixel.setPixelColor(0, pixel.Color(180, 0, 180));
    } else {
      pixel.setPixelColor(0, pixel.Color(0, 0, 0));
    }
    pixel.show();
  } else {
    pixel.setPixelColor(0, pixel.Color(0, 0, 0));
    pixel.show();
  }
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  updateLeds();
  updateLight();
  detectCharging();
}
