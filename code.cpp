/*
  ESP32 Energy Meter
  ------------------------------------------------------
  Architecture:
   - WiFi STA mode -> phone hotspot (PALASHシ)
   - NO web server, NO websockets (AsyncTCP/ESPAsyncWebServer removed)
   - PubSubClient -> publishes live JSON to campus/ee/meter1
   - Subscribes to campus/ee/meter1/cmd for TOGGLE / RESET / TIME:<epoch>
   - Smart Switchgear: Over-Voltage / Over-Current relay trip + fault flag
   - TFT + SD logging still on ESP32; heavy work (FFT/graphs/web) on phone
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// ==================== Hardware pins ====================
#define ZMPT101B_PIN  34
#define ACS712_PIN    35
#define RELAY_PIN     26
#define SD_CS          5

// ==================== WiFi (STA mode -> Phone Hotspot) ====================
const char* ssid     = "PALASH\xe3\x82\xb7";   // "PALASHシ" (UTF-8) <-- Edit if you want
const char* password = "PALASH69";   // <-- Edit if you want :))

// Phone (MQTT broker + Flask) IP — Android hotspot gateway is usually 192.168.43.1
const char* mqtt_server     = "192.168.43.1";
const int   mqtt_port       = 1883;
const char* mqtt_pub_topic  = "campus/ee/meter1";
const char* mqtt_sub_topic  = "campus/ee/meter1/cmd";
const char* mqtt_wave_topic = "campus/ee/meter1/waveform";

WiFiClient   espClient;
PubSubClient mqtt(espClient);
TFT_eSPI     tft = TFT_eSPI();

// ==================== Calibration ====================
float voltageCal    = 0.350f;
float currentCal    = 0.008f;
float voltageOffset = 2048.0f;
float currentOffset = 2048.0f;
float energyRate    = 7.00f;   // Rs per kWh

// ==================== Smart Switchgear (fault protection) ====================
#define OV_TRIP_V        270.0f   // over-voltage trip
#define OC_TRIP_A         10.0f   // over-current trip
#define FAULT_CLEAR_MS   30000UL  // auto-reclose after 30s normal
bool          faultActive = false;
unsigned long faultTrippedMs = 0;

// ==================== Live measurements ====================
float  voltageRMS = 0.0f, currentRMS = 0.0f;
float  powerReal = 0.0f, powerApparent = 0.0f, powerFactor = 0.0f;
double energyTotal = 0.0, costTotal = 0.0;
double todayEnergy = 0.0, monthEnergy = 0.0;
bool   relayState = false, sdOK = false, timeSynced = false;

String        latestJsonData = "{}";
unsigned long lastTftUpdate = 0;

// Open bucket accumulators (minute/hour averages for SD log)
float  mSumV = 0, mSumI = 0, mSumP = 0; double mE = 0; int mN = 0;
float  hSumV = 0, hSumI = 0, hSumP = 0; double hE = 0; int hN = 0;
unsigned long minStart = 0, hourStart = 0, lastReadingMs = 0;
int lastDay = -1, lastMonth = -1;

// ==================== Epoch time helper ====================
uint32_t nowEpoch() {
  time_t t = time(nullptr);
  if (t < 1600000000) return 0;
  return (uint32_t)t;
}

String tsToStr(uint32_t ep) {
  if (ep < 1600000000) return String("not-synced");
  char b[32];
  time_t t = ep; struct tm ti;
  localtime_r(&t, &ti);
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &ti);
  return String(b);
}

// ==================== Relay ====================
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH);
}

// ==================== JSON snapshot (live) ====================
void updateJSON() {
  String json;
  json.reserve(320);
  json = "{";
  json += "\"voltage\":" + String(voltageRMS, 1) + ",";
  json += "\"current\":" + String(currentRMS, 2) + ",";
  json += "\"power\":" + String(powerReal, 1) + ",";
  json += "\"apparent\":" + String(powerApparent, 1) + ",";
  json += "\"pf\":" + String(powerFactor, 2) + ",";
  json += "\"energy\":" + String(energyTotal, 3) + ",";
  json += "\"cost\":" + String(costTotal, 2) + ",";
  json += "\"today\":" + String(todayEnergy, 3) + ",";
  json += "\"month\":" + String(monthEnergy, 3) + ",";
  json += "\"now\":" + String(nowEpoch()) + ",";
  json += "\"sd\":" + String(sdOK ? "true" : "false") + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"fault\":" + String(faultActive ? "true" : "false");
  json += "}";
  latestJsonData = json;
}

// ==================== SD card logging ====================
void sdWriteHeader(const char* path, const char* header) {
  if (!sdOK) return;
  if (!SD.exists(path)) {
    File f = SD.open(path, FILE_WRITE);
    if (f) { f.println(header); f.close(); }
  }
}

void sdLogLine(const char* path, const String &line) {
  if (!sdOK) return;
  File f = SD.open(path, FILE_APPEND);
  if (f) { f.println(line); f.close(); }
}

void writeBillFile() {
  if (!sdOK) return;
  String b;
  b += "====== KUCCHUPUCCHU ENERGY BILL ======\n";
  b += "Date: " + tsToStr(nowEpoch()) + "\n";
  b += "Energy: " + String(energyTotal, 3) + " kWh\n";
  b += "Rate: Rs " + String(energyRate, 2) + "/kWh\n";
  b += "TOTAL: Rs " + String(energyTotal * energyRate, 2) + "\n";
  b += "Today: " + String(todayEnergy, 3) + " kWh\n";
  b += "Month: " + String(monthEnergy, 3) + " kWh\n";
  File f = SD.open("/bill.txt", FILE_WRITE);
  if (f) { f.print(b); f.close(); }
}

// ==================== MQTT callback (commands from phone) ====================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[64];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = 0;
  String msg = buf;

  if (msg == "TOGGLE") {
    if (!faultActive) setRelay(!relayState);
  } else if (msg == "RESET") {
    energyTotal = 0.0; costTotal = 0.0;
    todayEnergy = 0.0; monthEnergy = 0.0;
    lastReadingMs = millis();
    faultActive = false;
  } else if (msg.startsWith("TIME:")) {
    unsigned long epoch = msg.substring(5).toInt();
    if (epoch > 1600000000) {
      struct timeval tv; tv.tv_sec = epoch; tv.tv_usec = 0;
      settimeofday(&tv, NULL);
      timeSynced = true;
    }
  }
}

void mqttReconnect() {
  while (!mqtt.connected()) {
    tft.fillRect(0, 100, 160, 28, TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(4, 104);
    tft.print("MQTT retry...");
    if (mqtt.connect("kucchupucchu-meter1")) {
      mqtt.subscribe(mqtt_sub_topic);
      mqtt.publish(mqtt_pub_topic, "{\"type\":\"boot\"}");
      break;
    }
    delay(2000);
  }
}

// ==================== Sensors ====================
void readSensors() {
  const int samples = 800;
  float sumV_sq = 0.0f;
  float sumI_sq = 0.0f;
  float sumP = 0.0f;

  long sum_rawV = 0;
  long sum_rawI = 0;

  for (int n = 0; n < samples; n++) {
    int rawV = analogRead(ZMPT101B_PIN);
    int rawI = analogRead(ACS712_PIN);

    sum_rawV += rawV;
    sum_rawI += rawI;

    float vInst = (rawV - voltageOffset) * voltageCal;
    float iInst = (rawI - currentOffset) * currentCal;

    sumV_sq += vInst * vInst;
    sumI_sq += iInst * iInst;
    sumP += vInst * iInst;

    delayMicroseconds(125);
  }

  voltageRMS = sqrtf(sumV_sq / samples);
  float rawCurrentRMS = sqrtf(sumI_sq / samples);

  float noiseFloor = 0.18f;
  if (rawCurrentRMS > noiseFloor) {
    currentRMS = sqrtf((rawCurrentRMS * rawCurrentRMS) - (noiseFloor * noiseFloor));
  } else {
    currentRMS = 0.0f;
  }

  voltageOffset = (float)sum_rawV / samples;
  currentOffset = (float)sum_rawI / samples;

  voltageRMS = roundf(voltageRMS * 10.0f) / 10.0f;
  currentRMS = roundf(currentRMS * 100.0f) / 100.0f;

  if (voltageRMS < 10.0f) voltageRMS = 0.0f;
  if (currentRMS < 0.05f) currentRMS = 0.0f;

  float measuredRealPower = sumP / samples;
  if (measuredRealPower < 0.0f) measuredRealPower = -measuredRealPower;

  powerApparent = voltageRMS * currentRMS;

  if (powerApparent > 0.5f) {
    powerFactor = measuredRealPower / powerApparent;
    if (powerFactor > 1.0f) powerFactor = 1.0f;
    if (powerFactor < 0.0f) powerFactor = 0.0f;
  } else {
    powerFactor = 0.0f;
  }

  powerFactor = roundf(powerFactor * 100.0f) / 100.0f;
  powerReal = powerApparent * powerFactor;
}

// ==================== Smart Switchgear protection ====================
void updateFaultProtection() {
  if (!faultActive) {
    if (voltageRMS > OV_TRIP_V || currentRMS > OC_TRIP_A) {
      faultActive = true;
      faultTrippedMs = millis();
      setRelay(false);   // TRIP!
    }
  } else {
    if (millis() - faultTrippedMs > FAULT_CLEAR_MS &&
        voltageRMS < OV_TRIP_V * 0.95f &&
        currentRMS < OC_TRIP_A * 0.95f) {
      faultActive = false;
    }
  }
}

// ==================== Energy + SD logging engine ====================
void updateEnergyAndBuckets() {
  unsigned long now = millis();
  if (lastReadingMs == 0) lastReadingMs = now;
  float dtHours = (now - lastReadingMs) / 3600000.0f;
  lastReadingMs = now;

  if (powerReal < 0) powerReal = 0;
  double eDelta = (powerReal / 1000.0) * (double)dtHours;
  energyTotal += eDelta;
  todayEnergy += eDelta;
  monthEnergy += eDelta;
  costTotal = energyTotal * energyRate;

  if (timeSynced) {
    struct tm ti;
    if (getLocalTime(&ti)) {
      if (lastDay == -1) { lastDay = ti.tm_mday; lastMonth = ti.tm_mon; }
      if (ti.tm_mday != lastDay)   { todayEnergy = 0; lastDay = ti.tm_mday; }
      if (ti.tm_mon  != lastMonth) { monthEnergy = 0; lastMonth = ti.tm_mon; }
    }
  }

  uint32_t ep = nowEpoch();

  mSumV += voltageRMS; mSumI += currentRMS; mSumP += powerReal; mE += eDelta; mN++;

  if (now - minStart >= 60000ul) {
    if (mN > 0) {
      sdLogLine("/datalog.csv", tsToStr(ep) + "," + String(mSumV / mN, 1) + "," +
                String(mSumI / mN, 2) + "," + String(mSumP / mN, 1) + "," +
                String(mE, 5) + "," + String(energyTotal, 3));
      hSumV += mSumV / mN; hSumI += mSumI / mN; hSumP += mSumP / mN; hE += mE; hN++;
    }
    mSumV = mSumI = mSumP = 0; mE = 0; mN = 0; minStart = now;

    if (now - hourStart >= 3600000ul) {
      if (hN > 0) {
        sdLogLine("/hours.csv", tsToStr(ep) + "," + String(hSumV / hN, 1) + "," +
                  String(hSumI / hN, 2) + "," + String(hSumP / hN, 1) + "," + String(hE, 5));
        writeBillFile();
      }
      hSumV = hSumI = hSumP = 0; hE = 0; hN = 0; hourStart = now;
    }
  }
}

// ==================== TFT ====================
void updateTFT() {
  tft.fillRect(0, 0, 160, 128, TFT_BLACK);
  tft.fillRect(0, 0, 160, 20, TFT_NAVY);
  tft.setTextFont(2);
  tft.setCursor(4, 3);
  if (timeSynced) {
    struct tm ti;
    if (getLocalTime(&ti)) {
      tft.setTextColor(TFT_CYAN, TFT_NAVY);
      char b[16];
      strftime(b, sizeof(b), "%H:%M:%S", &ti);
      tft.print(b);
    }
  } else {
    tft.setTextColor(TFT_CYAN, TFT_NAVY);
    tft.print("ENERGY METER");
  }
  tft.setTextColor(sdOK ? TFT_GREEN : TFT_RED, TFT_NAVY);
  tft.setCursor(120, 3); tft.print(sdOK ? "SD" : "NO");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(4, 24);  tft.printf("V: %6.1f V", voltageRMS);
  tft.setCursor(4, 41);  tft.printf("I: %6.2f A", currentRMS);
  tft.setCursor(4, 58);  tft.printf("P: %6.1f W", powerReal);
  tft.setCursor(4, 75);  tft.printf("PF:    %0.2f", powerFactor);
  tft.setCursor(4, 92);  tft.printf("E: %6.3f kWh", energyTotal);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(4, 109); tft.printf("Today:%5.3f", todayEnergy);

  if (faultActive) {
    tft.fillRect(0, 109, 160, 19, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setCursor(4, 112);
    tft.print("FAULT! RELAY TRIP");
  }
}

// ==================== Waveform capture (Harmonic Analysis) ====================
#define WAVEFORM_SAMPLES     100
#define WAVEFORM_SAMPLE_US   200      // 200us -> 5kHz sample rate (20ms = 1x 50Hz cycle)
#define WAVEFORM_INTERVAL_MS 10000UL  // every 10 seconds

unsigned long lastWaveformMs = 0;

void publishWaveform() {
  int raw[WAVEFORM_SAMPLES];
  unsigned long t0 = micros();
  for (int n = 0; n < WAVEFORM_SAMPLES; n++) {
    raw[n] = analogRead(ZMPT101B_PIN);
    while (micros() - t0 < (unsigned long)(n + 1) * WAVEFORM_SAMPLE_US) { }
  }

  String json;
  json.reserve(1024);
  json = "{\"v_raw\":[";
  for (int n = 0; n < WAVEFORM_SAMPLES; n++) {
    json += String(raw[n]);
    if (n < WAVEFORM_SAMPLES - 1) json += ",";
  }
  json += "]}";

  mqtt.publish(mqtt_wave_topic, json.c_str());
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  setenv("TZ", "IST-5:30", 1);
  tzset();

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // ==================== SD Card (init before TFT) ====================
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);        // TFT CS high

  pinMode(19, INPUT_PULLUP);     // MISO pull-up

  SPI.begin(18, 19, 23, SD_CS);  // SCK, MISO, MOSI, CS

  sdOK = false;
  for (int attempt = 1; attempt <= 3 && !sdOK; attempt++) {
    Serial.printf("SD init attempt %d: ", attempt);
    if (SD.begin(SD_CS, SPI, 4000000)) {
      sdOK = true;
      Serial.println("OK");
    } else {
      Serial.println("FAIL");
      SD.end();
      delay(300);
      SPI.begin(18, 19, 23, SD_CS);
    }
  }

  if (sdOK) {
    sdWriteHeader("/datalog.csv", "Timestamp,V(V),I(A),P(W),Interval_kWh,Total_kWh");
    sdWriteHeader("/hours.csv",  "Timestamp,V_avg,I_avg,P_avg,kWh");
    writeBillFile();
    Serial.printf("SD Card OK: %llu MB\n", SD.cardSize() / 1024 / 1024);
  } else {
    Serial.println("SD NOT detected. Check: >32GB card? exFAT? D5 solder wire? weak 3.3V?");
  }

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 50);
  tft.println("Calibrating...");

  delay(2000);

  long vSum = 0, iSum = 0;
  const int calSamples = 2000;
  for (int n = 0; n < calSamples; n++) {
    vSum += analogRead(ZMPT101B_PIN);
    iSum += analogRead(ACS712_PIN);
    delayMicroseconds(500);
  }
  voltageOffset = (float)vSum / calSamples;
  currentOffset = (float)iSum / calSamples;

  // ==================== WiFi Station Mode ====================
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(8, 45);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("Connecting WiFi..");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); tries++;
    tft.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(5, 50);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("WiFi FAILED");
    return;
  }

  IPAddress ip = WiFi.localIP();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(4, 35); tft.print("Connected!");
  tft.setCursor(4, 55); tft.print("IP:"); tft.println(ip);
  tft.setCursor(4, 75); tft.print("Broker:");
  tft.setCursor(4, 90); tft.print(mqtt_server);
  delay(2500);

  // ==================== MQTT ====================
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqttReconnect();

  minStart = hourStart = lastReadingMs = millis();
  lastTftUpdate = millis();
  updateJSON();
  tft.fillScreen(TFT_BLACK);
}

// ==================== Main Loop ====================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    delay(5000);
  }
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  if (millis() - lastWaveformMs >= WAVEFORM_INTERVAL_MS) {
    lastWaveformMs = millis();
    publishWaveform();
  }

  readSensors();
  updateFaultProtection();
  updateEnergyAndBuckets();

  if (millis() - lastTftUpdate >= 1000) {
    updateJSON();
    updateTFT();
    mqtt.publish(mqtt_pub_topic, latestJsonData.c_str());
    lastTftUpdate = millis();
  }

  delay(10);
}
