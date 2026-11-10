
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include "wave_array.h"

// buoy_secrets.h is optional and not tracked by git. Copy
// buoy_secrets.example.h to buoy_secrets.h and fill it in, or leave it out
// and enter everything over the serial port on first boot.
#if __has_include("buoy_secrets.h")
#include "buoy_secrets.h"
#endif
#ifndef SECRET_SSID
#define SECRET_SSID ""
#endif
#ifndef SECRET_PASS
#define SECRET_PASS ""
#endif
#ifndef SECRET_KEY
#define SECRET_KEY ""
#endif
#ifndef SECRET_REF
#define SECRET_REF ""
#endif

struct waveFloats_t {
 float hmax = 1.0;   // modest defaults until the first good fetch
 float tp = 10;
};

const int nixiePin = 16;  //Output pin the IN-9 is controlled from

// Number of samples in wave_array[] (one full wave cycle).
const int WAVE_SAMPLES = sizeof(wave_array) / sizeof(wave_array[0]);

// The output full scale (PWM 255) is scaled to this wave height in metres.
// A compromise between accuracy in extreme conditions and not looking boring
// at normal (0.8-1.2 m) heights.
const float MAX_HEIGHT_M = 6.0;
const int PWM_MAX = 255;

// Readings outside these limits are treated as bad data (the feed can carry
// sentinel values such as 9999) and the previous reading is kept.
const float HEIGHT_LIMIT_M = 30.0;
const float PERIOD_MIN_S = 1.0;
const float PERIOD_MAX_S = 30.0;

// The API publishes a new observation every 30 minutes.
const unsigned long FETCH_INTERVAL_MS = 30UL * 60UL * 1000UL;
// After a failed fetch retry sooner: 1 minute, doubling on each further
// failure up to the normal interval.
const unsigned long RETRY_INTERVAL_MS = 60UL * 1000UL;

// With no good observation for this long the wave stops and the tube holds a
// steady low glow, so old data is not mistaken for live data.
const unsigned long STALE_AFTER_MS = 3UL * 60UL * 60UL * 1000UL;
const int STALE_PWM = 20;   // adjust to a level your tube holds steadily

// Last good observation. Kept whenever a fetch fails so a short network
// outage keeps the last real wave rather than jumping to full scale or
// stopping dead.
waveFloats_t lastGood;
bool haveGoodData = false;
bool dataStale = false;
unsigned long lastGoodMillis = 0;

bool haveFetched = false;
unsigned long lastFetchMillis = 0;
unsigned long fetchInterval = FETCH_INTERVAL_MS;
unsigned long retryInterval = RETRY_INTERVAL_MS;

// ---------------------------------------------------------------------------
// Credentials
//
// Values come from buoy_secrets.h (if present and not still the "..._HERE"
// placeholders) or are asked for over the USB serial port (115200 baud, send
// a newline after each answer) and saved to flash, so they survive power
// cycles. To change saved values, press Enter in the serial monitor within
// the first few seconds after boot.
//
// Whichever was changed most recently wins: values entered over serial
// override buoy_secrets.h, until the sketch is re-flashed with a different
// buoy_secrets.h, at which point the header takes over again.
// ---------------------------------------------------------------------------

struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char key[65];
  char ref[129];
  uint32_t secretsHash;   // hash of the compiled-in values when last saved
};

const uint32_t CONFIG_MAGIC = 0xB0E7C0DE;
const unsigned long BOOT_WINDOW_MS = 5000UL;        // time to press Enter at boot
const unsigned long WIFI_CONNECT_MS = 20000UL;      // time to wait for WiFi
const unsigned long REENTER_WINDOW_MS = 30000UL;    // after a failed connect
const unsigned long SETUP_IDLE_TIMEOUT_MS = 60000UL; // idle prompt gives up

Config cfg;

bool isPlaceholder(const char* v) {
  size_t n = strlen(v);
  return n == 0 || (n >= 5 && strcmp(v + n - 5, "_HERE") == 0);
}

void copyField(char* dst, size_t size, const char* src) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// The WiFi password is deliberately not required: empty means an open network.
bool configComplete() {
  return !isPlaceholder(cfg.ssid) && !isPlaceholder(cfg.key) &&
         !isPlaceholder(cfg.ref);
}

uint32_t fnv1a(uint32_t h, const char* s) {
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619UL;
  }
  h ^= 0xFF;            // field separator
  h *= 16777619UL;
  return h;
}

uint32_t compiledSecretsHash() {
  uint32_t h = 2166136261UL;
  h = fnv1a(h, SECRET_SSID);
  h = fnv1a(h, SECRET_PASS);
  h = fnv1a(h, SECRET_KEY);
  h = fnv1a(h, SECRET_REF);
  return h;
}

void saveConfig() {
  cfg.magic = CONFIG_MAGIC;
  cfg.secretsHash = compiledSecretsHash();
  EEPROM.put(0, cfg);
  if (EEPROM.commit()) {
    Serial.println("[Setup] saved to flash");
  } else {
    Serial.println("[Setup] WARNING: could not write to flash, settings will be lost at power off");
  }
}

// Use a compiled-in value when it is real and either nothing is saved for
// that field or buoy_secrets.h has changed since the last save.
void applyCompiled(char* field, size_t size, const char* compiled, bool headerChanged) {
  if (!isPlaceholder(compiled) && (headerChanged || isPlaceholder(field))) {
    copyField(field, size, compiled);
  }
}

void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  bool saved = cfg.magic == CONFIG_MAGIC;
  if (!saved) {
    memset(&cfg, 0, sizeof(cfg));
  }
  // Make sure every stored string is terminated, whatever was in flash.
  cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
  cfg.pass[sizeof(cfg.pass) - 1] = '\0';
  cfg.key[sizeof(cfg.key) - 1] = '\0';
  cfg.ref[sizeof(cfg.ref) - 1] = '\0';

  bool headerChanged = cfg.secretsHash != compiledSecretsHash();
  applyCompiled(cfg.ssid, sizeof(cfg.ssid), SECRET_SSID, headerChanged);
  applyCompiled(cfg.pass, sizeof(cfg.pass), SECRET_PASS, headerChanged);
  applyCompiled(cfg.key,  sizeof(cfg.key),  SECRET_KEY,  headerChanged);
  applyCompiled(cfg.ref,  sizeof(cfg.ref),  SECRET_REF,  headerChanged);

  // Remember the new header so later serial changes are not overridden by it
  // again on the next boot.
  if (saved && headerChanged) {
    Serial.println("[Setup] buoy_secrets.h changed since settings were saved; its values now apply");
    saveConfig();
  }
}

// Read one line from serial. timeoutMs = 0 waits forever, otherwise the read
// gives up after that long with no key pressed. When `mask` is true the
// characters are echoed as '*'. Returns false on timeout.
bool readLine(char* out, size_t size, unsigned long timeoutMs, bool mask) {
  // A line ending of "\r\n" can arrive split across two reads, so remember a
  // trailing '\r' and swallow the '\n' that follows it, even on a later call.
  static bool lastWasCR = false;
  size_t len = 0;
  unsigned long start = millis();
  while (timeoutMs == 0 || millis() - start < timeoutMs) {
    while (Serial.available()) {
      char c = Serial.read();
      start = millis();
      bool afterCR = lastWasCR;
      lastWasCR = (c == '\r');
      if (c == '\n' && afterCR) continue;
      if (c == '\r' || c == '\n') {
        out[len] = '\0';
        Serial.println();
        return true;
      }
      if ((c == '\b' || c == 127) && len > 0) {   // backspace
        len--;
        Serial.print("\b \b");
      } else if (c >= 32 && c < 127 && len < size - 1) {
        out[len++] = c;
        Serial.print(mask ? '*' : c);
      }
    }
    delay(10);   // keeps the watchdog fed
  }
  out[len] = '\0';
  Serial.println();
  return false;
}

// Ask for one value. An empty answer keeps the current value if there is one.
// An `optional` field may be left empty, and "-" clears a saved value.
// Returns false if the prompt timed out.
bool askField(const char* label, char* field, size_t size, bool mask, bool optional,
              unsigned long timeoutMs) {
  char buf[129];
  while (true) {
    bool have = !isPlaceholder(field);
    if (have && mask) {
      Serial.printf("%s [Enter keeps saved value%s]: ", label, optional ? ", - for none" : "");
    } else if (have) {
      Serial.printf("%s [Enter keeps \"%s\"]: ", label, field);
    } else if (optional) {
      Serial.printf("%s [Enter for none]: ", label);
    } else {
      Serial.printf("%s: ", label);
    }
    if (!readLine(buf, sizeof(buf) < size ? sizeof(buf) : size, timeoutMs, mask)) {
      return false;
    }
    if (optional && strcmp(buf, "-") == 0) {
      field[0] = '\0';
      return true;
    }
    if (buf[0] != '\0') {
      copyField(field, size, buf);
      return true;
    }
    if (have || optional) return true;
    Serial.println("  (required)");
  }
}

// Interactive setup over serial. Returns true if settings were saved.
bool runSetup() {
  // If there is already a usable config, an idle prompt gives up, so a stray
  // byte on the serial line cannot leave the display stuck here forever.
  // With nothing usable there is no point carrying on, so wait indefinitely.
  unsigned long timeout = configComplete() ? SETUP_IDLE_TIMEOUT_MS : 0;
  Config backup = cfg;

  Serial.println();
  Serial.println("===== submerge setup =====");
  bool ok =
    askField("WiFi name (SSID)", cfg.ssid, sizeof(cfg.ssid), false, false, timeout) &&
    askField("WiFi password", cfg.pass, sizeof(cfg.pass), true, true, timeout) &&
    askField("coastalmonitoring.org API key", cfg.key, sizeof(cfg.key), true, false, timeout) &&
    askField("Referer (e.g. https://www.yourdomain.com/)", cfg.ref, sizeof(cfg.ref), false, false, timeout);

  if (!ok) {
    cfg = backup;
    Serial.println("[Setup] timed out, keeping previous settings");
    return false;
  }
  saveConfig();
  return true;
}

// Wait for any key within `windowMs`. Returns true if one was pressed.
bool keyPressedWithin(unsigned long windowMs) {
  while (Serial.available()) Serial.read();   // drop boot noise
  unsigned long start = millis();
  while (millis() - start < windowMs) {
    if (Serial.available()) {
      while (Serial.available()) { Serial.read(); delay(2); }
      return true;
    }
    delay(10);
  }
  return false;
}

bool connectWiFi() {
  WiFi.disconnect();
  if (cfg.pass[0] == '\0') {
    WiFi.begin(cfg.ssid);   // open network
  } else {
    WiFi.begin(cfg.ssid, cfg.pass);
  }
  Serial.printf("[WiFi] connecting to \"%s\"", cfg.ssid);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[WiFi] %s\n", ok ? "connected" : "NOT connected");
  return ok;
}

void setup() {
  pinMode(nixiePin,OUTPUT);
  analogWriteFreq(200);       //PWM frequency can be adapted to your tube
  analogWriteRange(PWM_MAX);  //Core >=3.0 defaults to 255, older cores to 1023: pin it
  Serial.begin(115200);
  delay(100);

  WiFi.persistent(false);     // we store credentials ourselves
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  loadConfig();

  if (!configComplete()) {
    Serial.println("\n[Setup] credentials missing");
    runSetup();
  } else {
    Serial.printf("\n[Setup] press Enter within %lu s to change WiFi / API settings\n",
                  BOOT_WINDOW_MS / 1000);
    if (keyPressedWithin(BOOT_WINDOW_MS)) runSetup();
  }

  // If WiFi will not come up, offer to re-enter the details. With nobody on
  // the serial port the window just times out and the sketch carries on
  // retrying, so a display running on a plain USB charger still works.
  while (!connectWiFi()) {
    Serial.printf("[Setup] press Enter within %lu s to re-enter credentials\n",
                  REENTER_WINDOW_MS / 1000);
    if (!keyPressedWithin(REENTER_WINDOW_MS)) break;
    runSetup();
  }
}

// Read a JSON value that may be a number, a numeric string or null.
// Returns 0 when it is missing or not a number.
float jsonNumber(JsonVariantConst v) {
  if (v.is<float>()) return v.as<float>();
  if (v.is<const char*>()) return atof(v.as<const char*>());
  return 0.0f;
}

// Fetch the latest wave height / period from coastalmonitoring.org into
// lastGood. Returns false on any failure, leaving lastGood unchanged.
bool grab_data (){

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] not connected, keeping last data");
    return false;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
  client->setInsecure();   // no certificate check; fine for a hobby display
  HTTPClient https;

  Serial.print("[HTTPS] begin...\n");
  // "sensor=113" selects the buoy
  if (!https.begin(*client, "https://coastalmonitoring.org/observations/waves/latest.geojson?sensor=113&duration=0")) {
    Serial.printf("[HTTPS] Unable to connect\n");
    return false;
  }

  https.addHeader("X-API-Key", cfg.key);
  https.addHeader("Referer", cfg.ref);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  Serial.print("[HTTPS] GET...\n");
  int httpCode = https.GET();
  Serial.printf("Response code: %d\n", httpCode);
  Serial.printf("Content length: %d\n", https.getSize());

  if (httpCode != HTTP_CODE_OK) {
    if (httpCode > 0) {
      Serial.printf("[HTTPS] GET... unexpected code: %d\n", httpCode);
      if (httpCode == 401 || httpCode == 403) {
        Serial.println("[HTTPS] check the API key and Referer (reboot and press Enter to change them)");
      }
    } else {
      Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();
  Serial.println(payload);

  JsonDocument doc;   // local, so its memory is released between fetches
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return false;
  }

  JsonObjectConst features_0_properties = doc["features"][0]["properties"];
  // The API sends numbers as strings ("11.100"), and hmax is often null, in
  // which case fall back to hs (significant wave height). Note hs is a
  // smaller quantity than hmax, so the display reads lower when this happens.
  float hmax = jsonNumber(features_0_properties["hmax"]); // Value for waveheight
  if (!(hmax > 0.0f)) {
    hmax = jsonNumber(features_0_properties["hs"]);
    if (hmax > 0.0f) Serial.println("hmax missing, using hs instead");
  }
  float tp = jsonNumber(features_0_properties["tp"]);     // Value for time period

  if (!(hmax > 0.0f) || !(tp > 0.0f)) {
    Serial.println("hmax/hs/tp missing or zero, keeping last data");
    return false;
  }
  if (!(hmax <= HEIGHT_LIMIT_M) || !(tp >= PERIOD_MIN_S) || !(tp <= PERIOD_MAX_S)) {
    Serial.println("hmax/hs/tp outside plausible range, keeping last data");
    return false;
  }

  lastGood.hmax = hmax;
  lastGood.tp = tp;
  return true;
}

void loop() {

  // Refresh the observation every fetchInterval (and once at boot).
  if (!haveFetched || (millis() - lastFetchMillis) >= fetchInterval) {
    bool ok = grab_data();
    lastFetchMillis = millis();
    haveFetched = true;

    if (ok) {
      haveGoodData = true;
      dataStale = false;
      lastGoodMillis = lastFetchMillis;
      fetchInterval = FETCH_INTERVAL_MS;
      retryInterval = RETRY_INTERVAL_MS;
    } else {
      fetchInterval = retryInterval;
      retryInterval = min(retryInterval * 2, FETCH_INTERVAL_MS);
      Serial.printf("Fetch failed, retrying in %lu s\n", fetchInterval / 1000);
    }

    Serial.print("Waveheight: ");
    Serial.println(lastGood.hmax);
    Serial.print("Period: ");
    Serial.println(lastGood.tp);
  }

  if (haveGoodData && !dataStale && (millis() - lastGoodMillis) >= STALE_AFTER_MS) {
    dataStale = true;
    Serial.println("No fresh data for a long time, showing steady glow");
  }

  // Stale: hold a steady low glow instead of replaying an old wave.
  if (dataStale) {
    analogWrite(nixiePin, STALE_PWM);
    delay(1000);
    return;
  }

  // Play one full wave cycle: the period is split into WAVE_SAMPLES chunks.
  unsigned long sample_delay = (unsigned long)((lastGood.tp / WAVE_SAMPLES) * 1000.0f);
  if (sample_delay < 1) {
    sample_delay = 1;
  }

  const float scale = PWM_MAX / MAX_HEIGHT_M;   // metres -> PWM counts

  for (int i = 0; i < WAVE_SAMPLES; i++) {
    // wave_array runs -1..1; map to 0..hmax metres
    float display_value = (wave_array[i] * (lastGood.hmax / 2)) + (lastGood.hmax / 2);
    int pwm = (int)(display_value * scale);
    analogWrite(nixiePin, constrain(pwm, 0, PWM_MAX));
    delay(sample_delay);
  }
}
