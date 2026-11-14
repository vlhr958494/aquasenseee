/*****************************************************************
 * AQUASENSE: 6-parameter water screening - ONE TEST = ONE LOGGED ROW
 *
 * How it works
 *   1. Put the probes in the sample. The LCD shows live readings.
 *   2. Press the push button (GPIO27 -> GND). The device measures for 60 s
 *      (first 15 s = settling, ignored; remaining 45 s are averaged).
 *   3. The averaged result + verdict is saved in flash and uploaded ONCE
 *      to a Google Sheet (one row per test). No live streaming.
 *   4. If there is no Wi-Fi the result stays in flash and is uploaded
 *      automatically later. Uploaded tests are deleted from flash and the
 *      sheet ignores duplicates, so nothing is sent twice.
 *
 * Physical : DS18B20 (GPIO4), SEN0244 TDS (GPIO35), SEN0161 pH (GPIO34),
 *            SEN0189 turbidity (GPIO32 via 10k/20k divider)
 * Derived  : EC (Eq. 3.1), DO (Eq. 3.2)
 * LCD      : 20x4 I2C (SDA GPIO21, SCL GPIO22), powered from 5 V
 *****************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <math.h>
#include <time.h>

struct WiFiNetwork {
  const char* ssid;
  const char* password;
};
#include "secrets.h"      // Wi-Fi list, Google script URL, token, device id

// --- TEST SETTINGS ---
#define START_BUTTON_PIN 27                      // external push button: one leg to GPIO27, other leg to GND
const unsigned long TEST_DURATION_MS = 60000;    // total test length
const unsigned long SETTLE_MS        = 15000;    // ignored at the start while probes settle
const bool AUTO_START_FIRST_TEST     = false;    // true = start one test automatically 10 s after power-up

// --- WI-FI / UPLOAD ---
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;   // per network
const unsigned long WIFI_RETRY_PAUSE_MS     = 30000;   // pause after trying every network
const unsigned long UPLOAD_RETRY_MS         = 15000;

// --- PINS ---
#define ONEWIRE_BUS 4
#define TDS_PIN     35
#define PH_PIN      34
#define TURBID_PIN  32
#define I2C_SDA     21
#define I2C_SCL     22

// --- FILTERING ---
const int   SAMPLE_COUNT = 30;       // odd number
const float EMA_ALPHA    = 0.3f;
const float TURBID_VOLTAGE_DIVIDER_RATIO = 1.80f;   // 10k over 20k
const float KE_FACTOR = 0.5f;       // TDS -> EC factor (Eq. 3.1)

// --- CALIBRATION ---
float TDS_CAL   = 1.0729f;              // known_ppm / displayed_ppm
// pH: PH_SLOPE  = (4.0 - 7.0) / (V_at_pH4 - V_at_pH7)
//     PH_OFFSET = 7.0 - PH_SLOPE * V_at_pH7      (voltages in volts)
float PH_SLOPE  = -5.70f;
float PH_OFFSET = 21.34f;


// --- WHO LIMITS ---
const float PH_MIN = 6.5f,  PH_MAX = 8.5f;
const float TURB_MAX = 5.0f;
const float TDS_MAX  = 500.0f;
const float EC_MIN = 200.0f, EC_MAX = 1000.0f;   // use 800 for the stricter limit
const float DO_MIN = 5.0f;
const float TEMP_MIN = 20.0f, TEMP_MAX = 30.0f;

// ---------------------------------------------------------------
OneWire oneWire(ONEWIRE_BUS);
DallasTemperature tempSensor(&oneWire);
hd44780_I2Cexp lcd;
Preferences prefs;

float waterTempC  = 25.0f;
bool  tempOk      = false;
float smoothedTds = -1.0f, smoothedPh = -1.0f, smoothedNtu = -1.0f;
float smoothedEc  = -1.0f, smoothedDo = -1.0f;

struct TestResult {
  uint32_t boot, testNo, ts;
  float ph, tds, ntu, temp, ec, dox;
  bool  tempOk;
  uint8_t mask, passed;
  bool  safe;
};

enum State { ST_IDLE, ST_TESTING, ST_DONE };
State state = ST_IDLE;
unsigned long testStartMs = 0;
double sumPh, sumTds, sumNtu, sumTemp, sumEc, sumDo;
int nSamples = 0, nTemp = 0;
TestResult finalResult;
uint32_t bootId = 0, testCounter = 0;
int pending = 0;                       // results waiting in flash
enum UploadStatus { UPL_NONE, UPL_PENDING, UPL_SENDING, UPL_OK, UPL_FAILED };
UploadStatus uploadStatus = UPL_NONE;   // upload state of the most recent test
bool timeStarted = false;
volatile bool buttonFlag = false;
const char* QUEUE_FILE = "/pending.csv";

void IRAM_ATTR onButton() {
  static unsigned long last = 0;
  unsigned long n = millis();
  if (n - last > 250) { last = n; buttonFlag = true; }
}

// ---------------------------------------------------------------
//  Sensors
// ---------------------------------------------------------------
uint32_t medianMilliVolts(uint8_t pin) {
  uint32_t buf[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    buf[i] = analogReadMilliVolts(pin);
    delay(5);
  }
  for (int i = 1; i < SAMPLE_COUNT; i++) {
    uint32_t k = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = k;
  }
  return buf[SAMPLE_COUNT / 2];
}

float ema(float prev, float x) { return (prev < 0) ? x : prev + EMA_ALPHA * (x - prev); }

float calculateEC(float tds_ppm) {                       // Eq. 3.1
  if (tds_ppm < 0.0f) tds_ppm = 0.0f;
  return tds_ppm / KE_FACTOR;
}

float calculateDO(float temp_c, float tds_ppm) {         // Eq. 3.2 (Benson-Krause + salinity)
  double TK = temp_c + 273.15;
  double lnDO = -139.34411 + 1.575701e5 / TK - 6.642308e7 / (TK * TK)
                + 1.243800e10 / (TK * TK * TK) - 8.621949e11 / (TK * TK * TK * TK);
  double sal = tds_ppm / 1000.0;
  lnDO -= sal * (0.017674 - 10.754 / TK + 2140.7 / (TK * TK));
  double d = exp(lnDO);
  return d < 0 ? 0.0f : (float)d;
}

void sampleSensors() {
  // temperature: read the previous conversion, then start the next (non-blocking)
  float t = tempSensor.getTempCByIndex(0);
  tempSensor.requestTemperatures();
  tempOk = (t > -55.0f && t < 125.0f && t != 85.0f);
  waterTempC = tempOk ? t : 25.0f;     // 25 C only used for compensation if the sensor fails

  float tds_v = medianMilliVolts(TDS_PIN) / 1000.0f;
  // TDS OFFSET AMBIENT AIR
  if (tds_v < 0.15) {tds_v = 0.0f;}
  float vc = tds_v / (1.0f + 0.02f * (waterTempC - 25.0f));
  float tds = (133.42f * vc * vc * vc - 255.86f * vc * vc + 857.39f * vc) * 0.5f * TDS_CAL;
  if (tds < 0) tds = 0;
  smoothedTds = ema(smoothedTds, tds);

  float ph_v = medianMilliVolts(PH_PIN) / 1000.0f;
  float rawPh = constrain(PH_SLOPE * ph_v + PH_OFFSET, 0.0f, 14.0f);
  smoothedPh = ema(smoothedPh, rawPh);

  float turb_pin_v = medianMilliVolts(TURBID_PIN) / 1000.0f;
  float sensor_v = turb_pin_v * TURBID_VOLTAGE_DIVIDER_RATIO;
  float ntu;
  if (sensor_v >= 4.2f)      ntu = 0.0f;
  else if (sensor_v >= 2.5f) ntu = max(0.0f, -1120.4f * sensor_v * sensor_v + 5742.3f * sensor_v - 4352.9f);
  else                       ntu = 3000.0f;
  smoothedNtu = ema(smoothedNtu, ntu);

  smoothedEc = ema(smoothedEc, calculateEC(smoothedTds));
  smoothedDo = ema(smoothedDo, calculateDO(waterTempC, smoothedTds));
  Serial.printf("[CAL] tds_v=%.3f ph_v=%.3f raw_v=%.3f sensor_v=%.3f\n", tds_v, ph_v, turb_pin_v, sensor_v);
}

// WHO evaluation. A missing temperature sensor fails Temp and DO.
void evaluateResult(TestResult& r) {
  bool phPass   = r.ph >= PH_MIN && r.ph <= PH_MAX;
  bool tempPass = r.tempOk && r.temp >= TEMP_MIN && r.temp <= TEMP_MAX;
  bool tdsPass  = r.tds <= TDS_MAX;
  bool turbPass = r.ntu < TURB_MAX;
  bool ecPass   = r.ec >= EC_MIN && r.ec <= EC_MAX;
  bool doPass   = r.tempOk && r.dox >= DO_MIN;
  r.mask = (phPass) | (tempPass << 1) | (tdsPass << 2) | (turbPass << 3) | (ecPass << 4) | (doPass << 5);
  r.passed = phPass + tempPass + tdsPass + turbPass + ecPass + doPass;
  r.safe = (r.passed >= 5);
}

TestResult makeLive() {
  TestResult r{};
  r.ph = smoothedPh; r.tds = smoothedTds; r.ntu = smoothedNtu; r.temp = waterTempC;
  r.ec = smoothedEc; r.dox = smoothedDo; r.tempOk = tempOk;
  evaluateResult(r);
  return r;
}

// ---------------------------------------------------------------
//  LCD
// ---------------------------------------------------------------
void lcdRow(uint8_t row, const char* text) {
  char buf[21];
  snprintf(buf, sizeof buf, "%-20.20s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void showValues(const TestResult& r) {
  char line[40], tb[10], tp[10], dv[10];
  snprintf(tb, sizeof tb, r.ntu < 100.0f ? "%.1f" : "%.0f", r.ntu);
  if (r.tempOk) { snprintf(tp, sizeof tp, "%.1fC", r.temp); snprintf(dv, sizeof dv, "%.1f", r.dox); }
  else          { snprintf(tp, sizeof tp, "ERR");            snprintf(dv, sizeof dv, "ERR"); }
  snprintf(line, sizeof line, "pH:%.1f  TDS:%.0f", r.ph, r.tds);   lcdRow(0, line);
  snprintf(line, sizeof line, "Turb:%s Temp:%s", tb, tp);          lcdRow(1, line);
  snprintf(line, sizeof line, "DO:%s  EC:%.0f", dv, r.ec);         lcdRow(2, line);
}

void updateLcd() {
  char line[40];
  if (state == ST_DONE) {
    showValues(finalResult);
    int phase = (millis() / 3000) % 3;            // row 3 rotates every 3 seconds
    if (phase == 0) {
      snprintf(line, sizeof line, "PASSED:%d/6  %s", finalResult.passed, finalResult.safe ? "SAFE" : "UNSAFE");
    } else if (phase == 1) {
      if (uploadStatus == UPL_OK)
        snprintf(line, sizeof line, "UPLOAD: COMPLETE");
      else if (uploadStatus == UPL_FAILED || WiFi.status() != WL_CONNECTED)
        snprintf(line, sizeof line, "UPLOAD: FAILED");
      else
        snprintf(line, sizeof line, "UPLOADING...");
    } else {
      snprintf(line, sizeof line, "PRESS TO START TEST");
    }
  } else {
    showValues(makeLive());
    if (state == ST_TESTING) {
      unsigned long el = millis() - testStartMs;
      unsigned long left = el >= TEST_DURATION_MS ? 0 : (TEST_DURATION_MS - el) / 1000;
      snprintf(line, sizeof line, "TESTING... %lus left", left);
    } else {
      snprintf(line, sizeof line, "PRESS TO START TEST");
    }
  }
  lcdRow(3, line);

  // Wi-Fi indicator in the upper-right corner (columns 18-19 of row 0): WF = connected, XX = not connected
  lcd.setCursor(18, 0);
  lcd.print(WiFi.status() == WL_CONNECTED ? "WF" : "XX");
}

// ---------------------------------------------------------------
//  Local store (LittleFS): one CSV line per finished test
//  boot,test,ts,ph,tds,ntu,temp,ec,do,tempOk,mask,passed,safe
// ---------------------------------------------------------------
void recountPending() {
  pending = 0;
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return;
  while (f.available()) if (f.read() == '\n') pending++;
  f.close();
}

bool queueAppend(const TestResult& r) {
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (!f) return false;
  f.printf("%lu,%lu,%lu,%.2f,%.1f,%.2f,%.2f,%.1f,%.2f,%d,%d,%d,%d\n",
           (unsigned long)r.boot, (unsigned long)r.testNo, (unsigned long)r.ts,
           r.ph, r.tds, r.ntu, r.temp, r.ec, r.dox,
           r.tempOk ? 1 : 0, r.mask, r.passed, r.safe ? 1 : 0);
  f.close();
  recountPending();
  return true;
}

String queueFirstLine() {
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return String();
  String l = f.readStringUntil('\n');
  f.close();
  l.trim();
  return l;
}

void queueDropFirst() {
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return;
  f.readStringUntil('\n');
  String rest;
  while (f.available()) rest += (char)f.read();
  f.close();
  if (rest.length() == 0) LittleFS.remove(QUEUE_FILE);
  else { File w = LittleFS.open(QUEUE_FILE, "w"); w.print(rest); w.close(); }
  recountPending();
}

// ---------------------------------------------------------------
//  Wi-Fi (non-blocking, tries the listed networks in order)
// ---------------------------------------------------------------
bool timeValid() { return time(nullptr) > 1700000000L; }

void maintainWiFi() {
  static size_t idx = 0;
  static bool attempting = false, everTried = false;
  static unsigned long attemptStart = 0, cycleEnd = 0;
  const size_t count = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (attempting) {
      attempting = false;
      Serial.printf("Wi-Fi connected: %s  IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    if (!timeStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); timeStarted = true; }
    return;
  }

  if (!attempting) {
    if (everTried && idx == 0 && now - cycleEnd < WIFI_RETRY_PAUSE_MS) return;
    while (idx < count && WIFI_NETWORKS[idx].ssid[0] == '\0') idx++;
    if (idx >= count) { idx = 0; cycleEnd = now; everTried = true; return; }
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_NETWORKS[idx].ssid);
    WiFi.begin(WIFI_NETWORKS[idx].ssid, WIFI_NETWORKS[idx].password);
    attempting = true; everTried = true; attemptStart = now;
  } else if (now - attemptStart > WIFI_CONNECT_TIMEOUT_MS) {
    Serial.println("Wi-Fi attempt timed out.");
    attempting = false;
    WiFi.disconnect();
    idx++;
    if (idx >= count) { idx = 0; cycleEnd = now; }
  }
}

// ---------------------------------------------------------------
//  Upload to Google Sheets (Apps Script web app)
// ---------------------------------------------------------------
bool lineToJson(const String& line, String& out) {
  unsigned long boot, test, ts;
  float ph, tds, ntu, temp, ec, dox;
  int tOk, mask, passed, safe;
  if (sscanf(line.c_str(), "%lu,%lu,%lu,%f,%f,%f,%f,%f,%f,%d,%d,%d,%d",
             &boot, &test, &ts, &ph, &tds, &ntu, &temp, &ec, &dox, &tOk, &mask, &passed, &safe) != 13)
    return false;
  char tempStr[16], doStr[16], buf[420];
  if (tOk) { snprintf(tempStr, sizeof tempStr, "%.2f", temp); snprintf(doStr, sizeof doStr, "%.2f", dox); }
  else     { strcpy(tempStr, "null");                          strcpy(doStr, "null"); }
  snprintf(buf, sizeof buf,
           "{\"token\":\"%s\",\"device\":\"%s\",\"id\":\"b%lu-t%lu\",\"ts\":%lu,"
           "\"ph\":%.2f,\"tds\":%.1f,\"turb\":%.2f,\"temp\":%s,\"do\":%s,\"ec\":%.1f,"
           "\"mask\":%d,\"passed\":%d,\"safe\":%d}",
           GOOGLE_TOKEN, DEVICE_ID, boot, test, ts, ph, tds, ntu, tempStr, doStr, ec, mask, passed, safe);
  out = String(buf);
  return true;
}

// Apps Script answers a POST with a redirect; the real answer ("OK") is behind it.
bool postToGoogle(const String& json) {
  int code;
  String location;
  {
    WiFiClientSecure client;
    client.setInsecure();   // TLS encrypted; Google's certificate is not pinned (see README note)
    HTTPClient http;
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, GOOGLE_SCRIPT_URL)) return false;
    const char* keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(json);
    location = http.header("Location");
    if (code == 200) { String r = http.getString(); http.end(); return r.startsWith("OK"); }
    http.end();
  }
  if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && location.length() > 0) {
    WiFiClientSecure client2;
    client2.setInsecure();
    HTTPClient h2;
    h2.setTimeout(15000);
    if (!h2.begin(client2, location)) return false;
    int code2 = h2.GET();
    String resp = (code2 == 200) ? h2.getString() : String();
    h2.end();
    Serial.printf("[upload] reply %d: %s\n", code2, resp.substring(0, 60).c_str());
    return code2 == 200 && resp.startsWith("OK");
  }
  Serial.printf("[upload] failed, HTTP/transport code %d\n", code);
  return false;
}

void maintainUpload() {
  static unsigned long lastTry = 0;
  static bool triedOnce = false;
  if (pending == 0 || state == ST_TESTING || WiFi.status() != WL_CONNECTED) return;  // never upload while measuring
  unsigned long now = millis();
  if (triedOnce && now - lastTry < UPLOAD_RETRY_MS) return;
  triedOnce = true; lastTry = now;

  String line = queueFirstLine(), json;
  if (line.length() == 0) { recountPending(); return; }
  if (!lineToJson(line, json)) { Serial.println("[upload] corrupt record removed"); queueDropFirst(); return; }

  uploadStatus = UPL_SENDING;
  lcdRow(3, "UPLOADING...");
  if (postToGoogle(json)) {
    queueDropFirst();
    Serial.printf("[upload] test uploaded, %d left\n", pending);
    triedOnce = false;                 // send the next pending test right away
    if (pending == 0) uploadStatus = UPL_OK;
  } else {
    uploadStatus = UPL_FAILED;         // data stays in flash; retried automatically
  }
}

// ---------------------------------------------------------------
//  Test control
// ---------------------------------------------------------------
void startTest() {
  sumPh = sumTds = sumNtu = sumTemp = sumEc = sumDo = 0;
  nSamples = nTemp = 0;
  testCounter++;
  testStartMs = millis();
  uploadStatus = UPL_NONE;
  state = ST_TESTING;
  Serial.printf("=== TEST %lu STARTED (%lu s) ===\n", (unsigned long)testCounter, TEST_DURATION_MS / 1000);
}

void accumulateSample() {
  sumPh += smoothedPh; sumTds += smoothedTds; sumNtu += smoothedNtu; sumEc += smoothedEc;
  nSamples++;
  if (tempOk) { sumTemp += waterTempC; sumDo += smoothedDo; nTemp++; }
}

void finishTest() {
  if (nSamples == 0) { state = ST_IDLE; return; }
  TestResult r{};
  r.boot = bootId; r.testNo = testCounter;
  r.ts = timeValid() ? (uint32_t)time(nullptr) : 0;
  r.ph = sumPh / nSamples; r.tds = sumTds / nSamples; r.ntu = sumNtu / nSamples; r.ec = sumEc / nSamples;
  r.tempOk = (nTemp >= nSamples * 0.8);
  r.temp = nTemp ? sumTemp / nTemp : 25.0f;
  r.dox  = nTemp ? sumDo / nTemp : 0.0f;
  evaluateResult(r);
  finalResult = r;
  state = ST_DONE;

  Serial.printf("=== TEST %lu DONE (%d samples) pH %.2f | TDS %.0f | Turb %.2f | Temp %.1f | DO %.2f | EC %.0f | %d/6 %s ===\n",
                (unsigned long)r.testNo, nSamples, r.ph, r.tds, r.ntu, r.temp, r.dox, r.ec, r.passed,
                r.safe ? "SAFE" : "UNSAFE");
  if (queueAppend(r)) uploadStatus = UPL_PENDING;
  else { Serial.println("ERROR: could not save the result to flash!"); uploadStatus = UPL_FAILED; }
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  delay(1000);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(500);
  int status = lcd.begin(20, 4);
  if (status) Serial.printf("LCD init FAILED (status %d). Check 5V power, SDA/SCL wiring.\n", status);
  else { lcd.backlight(); lcd.clear(); lcdRow(0, "AQUASENSE"); lcdRow(1, "Water potability"); lcdRow(2, "test logger"); lcdRow(3, "Initializing..."); }

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  prefs.begin("aqs", false);
  bootId = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootId);
  recountPending();

  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(START_BUTTON_PIN), onButton, FALLING);

  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN,    ADC_11db);
  analogSetPinAttenuation(PH_PIN,     ADC_11db);
  analogSetPinAttenuation(TURBID_PIN, ADC_11db);

  Serial.printf("AQUASENSE boot #%lu, %d unsent test(s) in flash\n", (unsigned long)bootId, pending);
  delay(1500);
  lcd.clear();
}

void loop() {
  static bool autoStarted = false;
  maintainWiFi();
  sampleSensors();
  unsigned long now = millis();

  if (buttonFlag) {
    buttonFlag = false;
    if (state != ST_TESTING) startTest();
  }
  if (AUTO_START_FIRST_TEST && !autoStarted && now > 10000) {
    autoStarted = true;
    if (state == ST_IDLE) startTest();
  }

  if (state == ST_TESTING) {
    unsigned long el = now - testStartMs;
    if (el >= SETTLE_MS) accumulateSample();
    if (el >= TEST_DURATION_MS) finishTest();
  }

  updateLcd();
  Serial.printf("[%s] pH:%.2f TDS:%.0f Turb:%.2f Temp:%s EC:%.0f DO:%.2f | pending:%d wifi:%d\n",
                state == ST_TESTING ? "TEST" : (state == ST_DONE ? "DONE" : "IDLE"),
                smoothedPh, smoothedTds, smoothedNtu, tempOk ? String(waterTempC, 1).c_str() : "ERR",
                smoothedEc, smoothedDo, pending, WiFi.status() == WL_CONNECTED);
  
  maintainUpload();
  delay(100);
}
