#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <MPU6050_tockn.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// =========================
// Device and cloud settings
// =========================
#define WIFI_SSID "AJA"
#define WIFI_PASSWORD "KAMY#2026$"

#define FIREBASE_API_KEY "AIzaSyCmW3A9lypdBna_lNz6hmlhAVBPkTCpN1A"
#define FIREBASE_DATABASE_URL "https://dvt-monitoring-system-default-rtdb.firebaseio.com"

// Create this Firebase Auth user from the admin dashboard, then keep the same values here.
#define DEVICE_ID "device_001"
#define PATIENT_ID "patient_001"
#define DEVICE_EMAIL "device_001@dvt.local"
#define DEVICE_PASSWORD "ChangeMe#2026"

// =========================
// Original hardware pin map
// =========================
#define EMG_PIN 34
#define DETECT_PIN 35
#define ONE_WIRE_BUS 27

#define BTN_BLUE_PIN 33
#define BTN_RED_PIN 26
#define BTN_GREEN_PIN 32

#define BUZZER_PIN 5
#define LED_RED_PIN 18
#define LED_GREEN_PIN 19

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
MPU6050 mpu6500(Wire);

FirebaseData fbdo;
FirebaseAuth firebaseAuth;
FirebaseConfig firebaseConfig;

const int RMS_WINDOW = 50;
int emgBuffer[RMS_WINDOW];
int bufferIndex = 0;

const int SMOOTH_SIZE = 10;
float smoothBuffer[SMOOTH_SIZE];
int smoothIndex = 0;

const int EMG_WAVE_POINTS = 60;
float emgWaveform[EMG_WAVE_POINTS];
int emgWaveIndex = 0;

const int BUZZER_TEST_LOW_FREQ = 2500;
const int BUZZER_TEST_HIGH_FREQ = 4200;
const int BUZZER_ALERT_FREQ = 4000;
const unsigned long BUZZER_ALERT_PERIOD = 280;
const unsigned long BUZZER_ALERT_ON_TIME = 240;

const unsigned long EMG_PREP_DURATION = 5000;
const unsigned long EMG_CALIB_DURATION = 5000;
const unsigned long EMG_SAMPLE_INTERVAL = 10;

float EMG_Rest = 0;
float EMG_Max = 0;

bool walkCalibrated = false;
const int WALK_SAMPLES = 100;
float walkSamples[WALK_SAMPLES];
float walkMotionSamples[WALK_SAMPLES];
int walkSampleIndex = 0;

const int SAMPLES = 300;
const int READ_INTERVAL = 100;
const int VAR_WINDOW = 10;
const float THRESHOLD_SIGMA = 3.0;
const float MOTION_STABLE_DEADBAND = 0.04;
const float MOTION_SCALE = 4.0;
const float MOTION_AXIS_DEADBAND = 0.015;
const float MOTION_AXIS_SCALE = 6.0;
const float MOTION_BOOST_LIMIT = 2.50;
const float MOTION_BOOST_FACTOR = 1.10;
const float MOTION_REST_LIMIT = 0.06;
const float MOTION_REST_PEAK_LIMIT = 0.25;
const float MOTION_BED_LIMIT = 1.50;
const float MOTION_MAX_VALUE = 5.0;
const int MOTION_SMOOTH_WINDOW = 8;

float accelSamples[SAMPLES];
int sampleIndex = 0;
float baselineMean = 0;
float baselineStd = 0;
float magWindow[VAR_WINDOW];
int windowIndex = 0;

bool mpuCalibrated = false;
bool emgCalibrated = false;

unsigned long lastSample = 0;
unsigned long lastUpdate = 0;
unsigned long lastMpuReadTime = 0;
unsigned long lastTempReadTime = 0;
unsigned long lastUploadTime = 0;
unsigned long lastAlarmPollTime = 0;
unsigned long lastStateSyncTime = 0;

float currentTemperature = 0.0;
float globalMotion = 0;
float globalMotionRaw = 0;
float globalMotionPeak = 0;
float globalVariance = 0;
float globalThreshold = 0;
bool globalIsMoving = false;
float latestEmgSignal = 0;
float latestEmgPercent = 0;
float latestMotionValue = 0;

float lastMotionAccX = 0;
float lastMotionAccY = 0;
float lastMotionAccZ = 0;
bool motionReferenceReady = false;
float motionSmoothWindow[MOTION_SMOOTH_WINDOW];
int motionSmoothIndex = 0;
int motionSmoothCount = 0;

unsigned long lastBtnBlueTime = 0;
unsigned long lastBtnGreenTime = 0;
const unsigned long DEBOUNCE_DELAY = 250;
int displayMode = 0;

unsigned long btnRedPressStartTime = 0;
bool btnRedIsPressed = false;
bool alarmTestActive = false;

bool cloudAlarmActive = false;
bool cloudConnected = false;

float calcRMS(int arr[], int len) {
  float sumSq = 0;
  for (int i = 0; i < len; i++) sumSq += (float)arr[i] * arr[i];
  return sqrt(sumSq / len);
}

float getSmoothed(float val) {
  smoothBuffer[smoothIndex] = val;
  smoothIndex = (smoothIndex + 1) % SMOOTH_SIZE;
  float sum = 0;
  for (int i = 0; i < SMOOTH_SIZE; i++) sum += smoothBuffer[i];
  return sum / SMOOTH_SIZE;
}

int readRectifiedEmg() {
  int raw = analogRead(EMG_PIN);
  int rect = abs(raw - 2048);
  if (rect < 20) rect = 0;
  return rect;
}

void resetEmgSignalBuffers(float value) {
  for (int i = 0; i < RMS_WINDOW; i++) emgBuffer[i] = (int)value;
  for (int i = 0; i < SMOOTH_SIZE; i++) smoothBuffer[i] = value;
  bufferIndex = 0;
  smoothIndex = 0;
}

float updateEmgRmsSample() {
  emgBuffer[bufferIndex] = readRectifiedEmg();
  bufferIndex = (bufferIndex + 1) % RMS_WINDOW;
  return calcRMS(emgBuffer, RMS_WINDOW);
}

float calculateEmgPercent(float current) {
  float range = EMG_Max - EMG_Rest;
  if (range < 1.0) range = 1.0;
  float pct = ((current - EMG_Rest) / range) * 100.0;
  return constrain(pct, 0, 100);
}

void resetEmgWaveform() {
  for (int i = 0; i < EMG_WAVE_POINTS; i++) emgWaveform[i] = 0;
  emgWaveIndex = 0;
}

void updateEmgWaveform(float percent) {
  emgWaveform[emgWaveIndex] = constrain(percent, 0, 100);
  emgWaveIndex = (emgWaveIndex + 1) % EMG_WAVE_POINTS;
}

float calculateMean(float arr[], int len) {
  float sum = 0;
  for (int i = 0; i < len; i++) sum += arr[i];
  return sum / len;
}

float calculateStd(float arr[], int len, float mean) {
  float sumSq = 0;
  for (int i = 0; i < len; i++) sumSq += pow(arr[i] - mean, 2);
  return sqrt(sumSq / len);
}

float calculateVariance(float arr[], int len) {
  if (len < 2) return 0;
  float mean = calculateMean(arr, len);
  float sumSq = 0;
  for (int i = 0; i < len; i++) sumSq += pow(arr[i] - mean, 2);
  return sumSq / (len - 1);
}

float getAccelMagnitude() {
  mpu6500.update();
  float x = mpu6500.getAccX();
  float y = mpu6500.getAccY();
  float z = mpu6500.getAccZ();
  return sqrt(x * x + y * y + z * z);
}

void resetMotionReference() {
  lastMotionAccX = 0;
  lastMotionAccY = 0;
  lastMotionAccZ = 0;
  motionReferenceReady = false;
  globalMotion = 0;
  globalMotionRaw = 0;
  globalMotionPeak = 0;
  motionSmoothIndex = 0;
  motionSmoothCount = 0;
  for (int i = 0; i < MOTION_SMOOTH_WINDOW; i++) motionSmoothWindow[i] = 0;
}

float calculateMotionLevel(float mag, float x, float y, float z) {
  float magnitudeShift = fabs(mag - baselineMean);
  float magMotion = 0.0;
  if (magnitudeShift > MOTION_STABLE_DEADBAND) {
    magMotion = (magnitudeShift - MOTION_STABLE_DEADBAND) * MOTION_SCALE;
  }

  if (!motionReferenceReady) {
    lastMotionAccX = x;
    lastMotionAccY = y;
    lastMotionAccZ = z;
    motionReferenceReady = true;
    return 0.0;
  }

  float axisDelta = sqrt(pow(x - lastMotionAccX, 2) + pow(y - lastMotionAccY, 2) + pow(z - lastMotionAccZ, 2));
  lastMotionAccX = x;
  lastMotionAccY = y;
  lastMotionAccZ = z;

  float axisMotion = 0.0;
  if (axisDelta > MOTION_AXIS_DEADBAND) {
    axisMotion = (axisDelta - MOTION_AXIS_DEADBAND) * MOTION_AXIS_SCALE;
  }

  float motion = max(magMotion, axisMotion);
  if (motion <= 0.0) return 0.0;
  if (motion > MOTION_BOOST_LIMIT) {
    motion = MOTION_BOOST_LIMIT + ((motion - MOTION_BOOST_LIMIT) * MOTION_BOOST_FACTOR);
  }
  return constrain(motion, 0.0, MOTION_MAX_VALUE);
}

float smoothMotionLevel(float rawMotion) {
  motionSmoothWindow[motionSmoothIndex] = constrain(rawMotion, 0.0, MOTION_MAX_VALUE);
  motionSmoothIndex = (motionSmoothIndex + 1) % MOTION_SMOOTH_WINDOW;
  if (motionSmoothCount < MOTION_SMOOTH_WINDOW) motionSmoothCount++;

  float sum = 0;
  globalMotionPeak = 0;
  for (int i = 0; i < motionSmoothCount; i++) {
    sum += motionSmoothWindow[i];
    if (motionSmoothWindow[i] > globalMotionPeak) globalMotionPeak = motionSmoothWindow[i];
  }
  if (motionSmoothCount == 0) return 0;
  return sum / motionSmoothCount;
}

float readMpuMotionLevel(float* magOut) {
  mpu6500.update();
  float x = mpu6500.getAccX();
  float y = mpu6500.getAccY();
  float z = mpu6500.getAccZ();
  float mag = sqrt(x * x + y * y + z * z);
  if (magOut) *magOut = mag;
  return calculateMotionLevel(mag, x, y, z);
}

const char* getMotionClass(float motion, float peak) {
  if (motion < MOTION_REST_LIMIT && peak < MOTION_REST_PEAK_LIMIT) return "REST";
  if (motion < MOTION_BED_LIMIT) return "BED";
  return "WALK";
}

bool isValidTemperature(float temp) {
  return (temp > 0 && temp != -127.00);
}

String deviceRootPath() {
  return String("/devices/") + DEVICE_ID;
}

bool firebaseReady() {
  return WiFi.status() == WL_CONNECTED && Firebase.ready();
}

String isoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 20)) {
    return String("millis_") + String(millis());
  }
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

void drawBoot() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(28, 18, "DVT GUARD AI");
  u8g2.drawStr(14, 35, "ESP32 SENSOR NODE");
  u8g2.drawStr(17, 52, "Cloud Decision Mode");
  u8g2.sendBuffer();
}

void drawWifiStatus(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 23, line1);
  u8g2.drawStr(8, 43, line2);
  u8g2.sendBuffer();
}

void drawNoSensorMessage(const char* subMsg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(15, 20, "CHECK ELECTRODES!");
  u8g2.drawStr(10, 40, subMsg);
  if ((millis() / 300) % 2 == 0) u8g2.drawFrame(0, 0, 128, 64);
  u8g2.sendBuffer();
}

void drawCountdown(int seconds) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(15, 20, "ELECTRODES DETECTED");
  u8g2.drawStr(10, 38, "Wait 5s to Prepare...");
  u8g2.setFont(u8g2_font_10x20_tf);
  char buf[4];
  sprintf(buf, "%d", seconds);
  u8g2.drawStr(60, 58, buf);
  u8g2.sendBuffer();
}

void drawMpuCalibrating(int pct) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(15, 15, "1. MPU MEASURE");
  u8g2.drawStr(20, 30, "Keep device still");
  u8g2.drawFrame(10, 40, 108, 12);
  int fillWidth = map(pct, 0, 100, 0, 106);
  u8g2.drawBox(11, 41, fillWidth, 10);
  char buf[10];
  sprintf(buf, "%d%%", pct);
  u8g2.drawStr(50, 60, buf);
  u8g2.sendBuffer();
}

void drawEmgPrepScreen(const char* title, const char* instruction, int seconds) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 13);
  u8g2.setDrawColor(0);
  u8g2.drawStr(4, 10, title);
  u8g2.setDrawColor(1);
  u8g2.drawStr(6, 27, instruction);
  u8g2.drawStr(16, 42, "Measure starts");
  u8g2.setFont(u8g2_font_10x20_tf);
  char buf[4];
  sprintf(buf, "%d", seconds);
  u8g2.drawStr(59, 63, buf);
  u8g2.sendBuffer();
}

void drawEmgCalibrating(const char* title, const char* instruction, int pct) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 13);
  u8g2.setDrawColor(0);
  u8g2.drawStr(4, 10, title);
  u8g2.setDrawColor(1);
  u8g2.drawStr(4, 29, instruction);
  u8g2.drawFrame(10, 40, 108, 12);
  int fillWidth = map(constrain(pct, 0, 100), 0, 100, 0, 106);
  u8g2.drawBox(11, 41, fillWidth, 10);
  char buf[10];
  sprintf(buf, "%d%%", pct);
  u8g2.drawStr(50, 62, buf);
  u8g2.sendBuffer();
}

void drawEmgCompleteScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 13);
  u8g2.setDrawColor(0);
  u8g2.drawStr(4, 10, "Measure Complete");
  u8g2.setDrawColor(1);
  u8g2.drawStr(12, 29, "MVC normalization");
  u8g2.setCursor(8, 45);
  u8g2.print("Rest: ");
  u8g2.print((int)EMG_Rest);
  u8g2.setCursor(8, 58);
  u8g2.print("Max : ");
  u8g2.print((int)EMG_Max);
  u8g2.sendBuffer();
}

void drawWalkCalibrating(int pct) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(15, 15, "3. WALK MEASURE");
  u8g2.drawStr(10, 30, "Please walk / move...");
  u8g2.drawFrame(10, 40, 108, 12);
  int fillWidth = map(pct, 0, 100, 0, 106);
  u8g2.drawBox(11, 41, fillWidth, 10);
  char buf[10];
  sprintf(buf, "%d%%", pct);
  u8g2.drawStr(50, 60, buf);
  u8g2.sendBuffer();
}

void drawAlarmTestScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(20, 20, "--- SELF TEST ---");
  u8g2.drawBox(5, 30, 118, 15);
  u8g2.setDrawColor(0);
  u8g2.drawStr(18, 41, "HOLD ALARM TEST");
  u8g2.setDrawColor(1);
  u8g2.drawStr(12, 58, "Release red to stop");
  u8g2.sendBuffer();
}

void drawCloudAlarmScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 17);
  u8g2.setDrawColor(0);
  u8g2.drawStr(17, 12, "DVT RISK ALARM");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr(22, 42, "ALARM");
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(7, 57);
  u8g2.print("Press red button to ack");
  if ((millis() / 180) % 2 == 0) u8g2.drawFrame(0, 0, 128, 64);
  u8g2.sendBuffer();
}

void drawEmgSignalBox(int x, int y, int w, int h) {
  u8g2.drawFrame(x, y, w, h);
  int prevX = -1;
  int prevY = -1;
  for (int i = 0; i < EMG_WAVE_POINTS; i++) {
    int index = (emgWaveIndex + i) % EMG_WAVE_POINTS;
    int px = x + 1 + (i * (w - 2)) / (EMG_WAVE_POINTS - 1);
    int py = y + h - 2 - (int)((constrain(emgWaveform[index], 0, 100) * (h - 3)) / 100.0);
    if (prevX >= 0) u8g2.drawLine(prevX, prevY, px, py);
    else u8g2.drawPixel(px, py);
    prevX = px;
    prevY = py;
  }
}

void drawCloudMonitor(int percent, float emgSignal, bool isMoving, float motion, const char* motionClass, float temp) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(4, 10, cloudConnected ? "CLOUD LINK OK" : "CLOUD OFFLINE");

  u8g2.setFont(u8g2_font_10x20_tf);
  char numStr[8];
  sprintf(numStr, "%d", percent);
  int x = 40 - (strlen(numStr) * 10) / 2;
  u8g2.setCursor(x, 30);
  u8g2.print(numStr);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(52, 30);
  u8g2.print("%");

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(5, 38);
  u8g2.print("SIG:");
  u8g2.print((int)emgSignal);
  u8g2.drawFrame(5, 40, 65, 8);
  int barFill = map(percent, 0, 100, 0, 61);
  u8g2.drawBox(7, 42, barFill, 4);
  drawEmgSignalBox(5, 49, 65, 14);

  u8g2.drawVLine(76, 16, 48);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(80, 22, "Motion");
  u8g2.setCursor(80, 31);
  u8g2.print("=");
  u8g2.print(motion, 2);

  if (isMoving) {
    u8g2.drawBox(80, 34, 44, 9);
    u8g2.setDrawColor(0);
    u8g2.drawStr(86, 41, motionClass);
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawFrame(80, 34, 44, 9);
    u8g2.drawStr(83, 41, "STABLE");
  }

  u8g2.drawStr(80, 51, "SKIN TEMP:");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(80, 62);
  if (!isValidTemperature(temp)) u8g2.print("ERR");
  else {
    u8g2.print(temp, 1);
    u8g2.print("C");
  }
  u8g2.sendBuffer();
}

void drawDebugMonitor(int percent, float emgSignal, float bLine, float cMax, float variance, float threshold, float motion, float temp) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(5, 9, "--- CLOUD SENSOR DEBUG ---");
  u8g2.setCursor(5, 21);
  u8g2.print("EMG:");
  u8g2.print(percent);
  u8g2.print("% S:");
  u8g2.print((int)emgSignal);
  u8g2.setCursor(5, 31);
  u8g2.print("Base/Max: ");
  u8g2.print((int)bLine);
  u8g2.print(" / ");
  u8g2.print((int)cMax);
  u8g2.setCursor(5, 43);
  u8g2.print("Motion = ");
  u8g2.print(motion, 3);
  u8g2.setCursor(5, 53);
  u8g2.print("V/T: ");
  u8g2.print(variance, 2);
  u8g2.print("/");
  u8g2.print(threshold, 2);
  u8g2.drawVLine(85, 14, 46);
  u8g2.drawStr(90, 22, "TEMP:");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(90, 33);
  u8g2.print(temp, 1);
  u8g2.print("C");
  u8g2.drawFrame(89, 44, 36, 14);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(94, 54, cloudConnected ? "CLOUD" : "LOCAL");
  u8g2.sendBuffer();
}

void driveAlarmOutputs(unsigned long now) {
  digitalWrite(LED_GREEN_PIN, LOW);
  bool pulse = (now % BUZZER_ALERT_PERIOD) < BUZZER_ALERT_ON_TIME;
  digitalWrite(LED_RED_PIN, pulse ? HIGH : LOW);
  if (pulse) tone(BUZZER_PIN, BUZZER_ALERT_FREQ);
  else noTone(BUZZER_PIN);
}

void driveNormalOutputs() {
  noTone(BUZZER_PIN);
  digitalWrite(LED_RED_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, HIGH);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawWifiStatus("Connecting WiFi", WIFI_SSID);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  drawWifiStatus("WiFi connected", WiFi.localIP().toString().c_str());
  delay(900);
}

void initFirebase() {
  firebaseConfig.api_key = FIREBASE_API_KEY;
  firebaseConfig.database_url = FIREBASE_DATABASE_URL;
  firebaseAuth.user.email = DEVICE_EMAIL;
  firebaseAuth.user.password = DEVICE_PASSWORD;
  Firebase.reconnectWiFi(true);
  Firebase.begin(&firebaseConfig, &firebaseAuth);
}

void pollCloudAlarm() {
  if (millis() - lastAlarmPollTime < 400) return;
  lastAlarmPollTime = millis();
  cloudConnected = firebaseReady();
  if (!cloudConnected) return;

  String path = deviceRootPath() + "/alarm/Active";
  if (Firebase.RTDB.getBool(&fbdo, path.c_str())) {
    cloudAlarmActive = fbdo.boolData();
  }
}

void acknowledgeCloudAlarm() {
  if (!firebaseReady()) {
    Serial.println("Cannot acknowledge alarm: Firebase is not ready.");
    return;
  }

  FirebaseJson ack;
  ack.set("Active", false);
  ack.set("AcknowledgedAt", isoTimestamp());
  ack.set("AcknowledgedAtMs/.sv", "timestamp");
  ack.set("AcknowledgedByDevice", DEVICE_ID);

  String path = deviceRootPath() + "/alarm";
  if (Firebase.RTDB.updateNode(&fbdo, path.c_str(), &ack)) {
    cloudAlarmActive = false;
    Serial.println("Cloud alarm acknowledged by red button.");
  } else {
    Serial.print("Alarm acknowledge failed: ");
    Serial.println(fbdo.errorReason());
  }
}

void syncDeviceState() {
  if (millis() - lastStateSyncTime < 5000) return;
  lastStateSyncTime = millis();
  if (!firebaseReady()) {
    cloudConnected = false;
    return;
  }
  cloudConnected = true;

  FirebaseJson stateJson;
  stateJson.set("online", true);
  stateJson.set("lastSeen", isoTimestamp());
  stateJson.set("lastSeenMs/.sv", "timestamp");
  stateJson.set("wifiRssi", WiFi.RSSI());
  stateJson.set("firmware", "dvt_esp32_firebase");

  String path = deviceRootPath() + "/state";
  Firebase.RTDB.updateNode(&fbdo, path.c_str(), &stateJson);
}

void uploadSensorReading() {
  if (millis() - lastUploadTime < 1000) return;
  lastUploadTime = millis();
  if (!firebaseReady()) {
    cloudConnected = false;
    return;
  }
  cloudConnected = true;

  FirebaseJson json;
  json.set("EMG_Signal", latestEmgSignal);
  json.set("Temperature", currentTemperature);
  json.set("Motion_Value", latestMotionValue);
  json.set("Timestamp", isoTimestamp());
  json.set("DeviceId", DEVICE_ID);
  json.set("PatientId", PATIENT_ID);

  String path = deviceRootPath() + "/sensors";
  if (!Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json)) {
    Serial.print("Sensor upload failed: ");
    Serial.println(fbdo.errorReason());
  }
}

void readControlButtons() {
  unsigned long currentTime = millis();

  if (digitalRead(BTN_BLUE_PIN) == LOW && currentTime - lastBtnBlueTime >= DEBOUNCE_DELAY) {
    displayMode = (displayMode + 1) % 2;
    lastBtnBlueTime = currentTime;
  }

  if (digitalRead(BTN_GREEN_PIN) == LOW && currentTime - lastBtnGreenTime >= DEBOUNCE_DELAY && !cloudAlarmActive) {
    mpuCalibrated = false;
    emgCalibrated = false;
    walkCalibrated = false;
    sampleIndex = 0;
    walkSampleIndex = 0;
    windowIndex = 0;
    EMG_Rest = 0;
    EMG_Max = 0;
    resetEmgWaveform();
    resetMotionReference();
    lastBtnGreenTime = currentTime;
  }

  int btnRedState = digitalRead(BTN_RED_PIN);
  if (btnRedState == LOW) {
    if (!btnRedIsPressed) {
      btnRedPressStartTime = currentTime;
      btnRedIsPressed = true;
    } else if ((currentTime - btnRedPressStartTime >= 1500) && !alarmTestActive && !cloudAlarmActive) {
      alarmTestActive = true;
      Serial.println("--- ALARM SELF TEST ACTIVATED ---");
    }
  } else {
    if (!btnRedIsPressed) return;
    unsigned long pressDuration = currentTime - btnRedPressStartTime;
    bool wasAlarmTestActive = alarmTestActive;
    btnRedIsPressed = false;
    alarmTestActive = false;

    if (pressDuration < 1500 && !wasAlarmTestActive && cloudAlarmActive) {
      acknowledgeCloudAlarm();
    }

    if (wasAlarmTestActive) {
      noTone(BUZZER_PIN);
      digitalWrite(LED_RED_PIN, LOW);
      Serial.println("--- ALARM SELF TEST STOPPED ---");
    }
  }
}

void checkAndBlockForElectrodes(bool isInitialBoot) {
  while (digitalRead(DETECT_PIN) == LOW) {
    readControlButtons();
    pollCloudAlarm();
    if (cloudAlarmActive) {
      driveAlarmOutputs(millis());
      drawCloudAlarmScreen();
    } else {
      driveNormalOutputs();
      if (isInitialBoot) drawNoSensorMessage("Please attach to start");
      else drawNoSensorMessage("Please attach electrodes");
    }
    delay(100);
  }

  for (int i = 5; i > 0; i--) {
    drawCountdown(i);
    unsigned long startWait = millis();
    while (millis() - startWait < 1000) {
      readControlButtons();
      pollCloudAlarm();
      if (digitalRead(DETECT_PIN) == LOW) {
        checkAndBlockForElectrodes(isInitialBoot);
        return;
      }
      if (cloudAlarmActive) {
        driveAlarmOutputs(millis());
        drawCloudAlarmScreen();
      }
      delay(20);
    }
  }
}

bool runEmgPrepPhase(const char* title, const char* instruction) {
  int totalSeconds = EMG_PREP_DURATION / 1000;
  for (int seconds = totalSeconds; seconds > 0; seconds--) {
    if (digitalRead(DETECT_PIN) == LOW) return false;
    drawEmgPrepScreen(title, instruction, seconds);

    unsigned long startWait = millis();
    while (millis() - startWait < 1000) {
      readControlButtons();
      pollCloudAlarm();
      if (cloudAlarmActive) return false;
      if (digitalRead(DETECT_PIN) == LOW) return false;
      delay(20);
    }
  }
  return true;
}

float collectRelaxedEmg() {
  unsigned long phaseStart = millis();
  unsigned long lastEmgSample = 0;
  unsigned long lastDraw = 0;
  float sum = 0;
  int count = 0;

  while (millis() - phaseStart < EMG_CALIB_DURATION) {
    readControlButtons();
    pollCloudAlarm();
    if (cloudAlarmActive || digitalRead(DETECT_PIN) == LOW) return -1;

    unsigned long now = millis();
    if (now - lastEmgSample >= EMG_SAMPLE_INTERVAL) {
      float rms = updateEmgRmsSample();
      sum += rms;
      count++;
      lastEmgSample = now;
    }

    if (now - lastDraw >= 100) {
      int pct = (int)(((now - phaseStart) * 100UL) / EMG_CALIB_DURATION);
      drawEmgCalibrating("Relax Measuring...", "Keep calf relaxed", pct);
      lastDraw = now;
    }
  }

  drawEmgCalibrating("Relax Measuring...", "Keep calf relaxed", 100);
  return (count > 0) ? (sum / count) : 0;
}

float collectMaximumEmg() {
  unsigned long phaseStart = millis();
  unsigned long lastEmgSample = 0;
  unsigned long lastDraw = 0;
  float maxValue = EMG_Rest;

  while (millis() - phaseStart < EMG_CALIB_DURATION) {
    readControlButtons();
    pollCloudAlarm();
    if (cloudAlarmActive || digitalRead(DETECT_PIN) == LOW) return -1;

    unsigned long now = millis();
    if (now - lastEmgSample >= EMG_SAMPLE_INTERVAL) {
      float rms = updateEmgRmsSample();
      if (rms > maxValue) maxValue = rms;
      lastEmgSample = now;
    }

    if (now - lastDraw >= 100) {
      int pct = (int)(((now - phaseStart) * 100UL) / EMG_CALIB_DURATION);
      drawEmgCalibrating("Maximum Contraction", "Contract calf muscle", pct);
      lastDraw = now;
    }
  }

  drawEmgCalibrating("Maximum Contraction", "Contract calf muscle", 100);
  return maxValue;
}

bool runEmgMvcCalibration() {
  if (!runEmgPrepPhase("Prepare: Relax", "Get ready to relax")) return false;
  resetEmgSignalBuffers(readRectifiedEmg());
  EMG_Rest = collectRelaxedEmg();
  if (EMG_Rest < 0) return false;

  if (!runEmgPrepPhase("Prepare: MVC", "Get ready to contract")) return false;
  resetEmgSignalBuffers(EMG_Rest);
  EMG_Max = collectMaximumEmg();
  if (EMG_Max < 0) return false;

  if (EMG_Max <= EMG_Rest + 1.0) EMG_Max = EMG_Rest + 1.0;
  resetEmgSignalBuffers(EMG_Rest);
  drawEmgCompleteScreen();
  delay(1500);
  return true;
}

void printLiveMeasurements(float percent, float emgSignal, float rawMotion, float motion, float peakMotion, float temp, const char* motionClass) {
  Serial.print("DATA,EMG=");
  Serial.print(percent, 1);
  Serial.print(",EmgSignal=");
  Serial.print(emgSignal, 1);
  Serial.print(",MotionRaw=");
  Serial.print(rawMotion, 3);
  Serial.print(",Motion=");
  Serial.print(motion, 3);
  Serial.print(",MotionPeak=");
  Serial.print(peakMotion, 3);
  Serial.print(",Temp=");
  if (isValidTemperature(temp)) Serial.print(temp, 1);
  else Serial.print("ERR");
  Serial.print(",MoveClass=");
  Serial.print(motionClass);
  Serial.print(",CloudAlarm=");
  Serial.println(cloudAlarmActive ? "true" : "false");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(DETECT_PIN, INPUT_PULLUP);
  pinMode(BTN_BLUE_PIN, INPUT_PULLUP);
  pinMode(BTN_RED_PIN, INPUT_PULLUP);
  pinMode(BTN_GREEN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  driveNormalOutputs();

  resetEmgWaveform();
  resetMotionReference();

  Wire.begin(21, 22);
  u8g2.begin();
  drawBoot();
  delay(1200);

  connectWiFi();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  initFirebase();

  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  checkAndBlockForElectrodes(true);

  mpu6500.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(10, 30, "Measuring Gyro...");
  u8g2.sendBuffer();
  mpu6500.calcGyroOffsets(true);
}

void loop() {
  unsigned long now = millis();

  readControlButtons();
  pollCloudAlarm();
  syncDeviceState();

  if (alarmTestActive) {
    drawAlarmTestScreen();
    digitalWrite(LED_GREEN_PIN, LOW);
    unsigned long testPhase = now % 1000;
    if (testPhase < 280) {
      digitalWrite(LED_RED_PIN, HIGH);
      tone(BUZZER_PIN, BUZZER_TEST_LOW_FREQ);
    } else if (testPhase >= 360 && testPhase < 640) {
      digitalWrite(LED_RED_PIN, HIGH);
      tone(BUZZER_PIN, BUZZER_TEST_HIGH_FREQ);
    } else {
      digitalWrite(LED_RED_PIN, LOW);
      noTone(BUZZER_PIN);
    }
    return;
  }

  if (cloudAlarmActive) {
    driveAlarmOutputs(now);
    drawCloudAlarmScreen();
    return;
  }

  driveNormalOutputs();

  if (digitalRead(DETECT_PIN) == LOW) {
    resetMotionReference();
    sampleIndex = 0;
    walkSampleIndex = 0;
    checkAndBlockForElectrodes(false);
    lastSample = millis();
    lastUpdate = millis();
    lastMpuReadTime = millis();
    return;
  }

  if (!mpuCalibrated) {
    if (now - lastMpuReadTime >= READ_INTERVAL && sampleIndex < SAMPLES) {
      float mag = getAccelMagnitude();
      accelSamples[sampleIndex] = mag;
      sampleIndex++;
      lastMpuReadTime = now;
      if (sampleIndex % 30 == 0) drawMpuCalibrating((sampleIndex * 100) / SAMPLES);
    }
    if (sampleIndex >= SAMPLES) {
      baselineMean = calculateMean(accelSamples, SAMPLES);
      baselineStd = calculateStd(accelSamples, SAMPLES, baselineMean);
      mpuCalibrated = true;
      resetMotionReference();
      sampleIndex = 0;
      lastSample = now;
    }
    return;
  }

  if (!emgCalibrated) {
    if (!runEmgMvcCalibration()) {
      checkAndBlockForElectrodes(false);
      return;
    }
    emgCalibrated = true;
    sampleIndex = 0;
    lastSample = millis();
    return;
  }

  if (!walkCalibrated) {
    if (walkSampleIndex < WALK_SAMPLES) {
      if (now - lastSample >= 100) {
        float rms = updateEmgRmsSample();
        float pct = calculateEmgPercent(getSmoothed(rms));
        float mag = 0;
        float motion = smoothMotionLevel(readMpuMotionLevel(&mag));

        walkSamples[walkSampleIndex] = pct;
        walkMotionSamples[walkSampleIndex] = motion;
        walkSampleIndex++;
        lastSample = now;

        if (walkSampleIndex % 10 == 0) drawWalkCalibrating((walkSampleIndex * 100) / WALK_SAMPLES);
      }
      return;
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(15, 25, "ALL MEASURE DONE!");
    u8g2.drawStr(9, 43, "Cloud upload starting");
    u8g2.sendBuffer();
    delay(1500);

    walkCalibrated = true;
    resetMotionReference();
    lastSample = now;
    lastUpdate = now;
    lastMpuReadTime = now;
    lastTempReadTime = now;
    windowIndex = 0;
  }

  if (now - lastTempReadTime >= 1000) {
    currentTemperature = sensors.getTempCByIndex(0);
    sensors.requestTemperatures();
    lastTempReadTime = now;
  }

  if (now - lastSample >= 10) {
    emgBuffer[bufferIndex] = readRectifiedEmg();
    bufferIndex = (bufferIndex + 1) % RMS_WINDOW;
    lastSample = now;
  }

  if (now - lastUpdate >= 100) {
    float mag = 0;
    globalMotionRaw = readMpuMotionLevel(&mag);
    globalMotion = smoothMotionLevel(globalMotionRaw);
    const char* motionClass = getMotionClass(globalMotion, globalMotionPeak);
    magWindow[windowIndex] = mag;
    windowIndex = (windowIndex + 1) % VAR_WINDOW;

    if (windowIndex == 0) {
      globalVariance = calculateVariance(magWindow, VAR_WINDOW);
      globalThreshold = baselineStd * baselineStd * THRESHOLD_SIGMA;
    }
    globalIsMoving = strcmp(motionClass, "REST") != 0;

    float rms = calcRMS(emgBuffer, RMS_WINDOW);
    latestEmgSignal = getSmoothed(rms);
    latestEmgPercent = calculateEmgPercent(latestEmgSignal);
    latestMotionValue = globalMotion;
    updateEmgWaveform(latestEmgPercent);

    printLiveMeasurements(latestEmgPercent, latestEmgSignal, globalMotionRaw, globalMotion, globalMotionPeak, currentTemperature, motionClass);

    if (displayMode == 0) {
      drawCloudMonitor((int)latestEmgPercent, latestEmgSignal, globalIsMoving, globalMotion, motionClass, currentTemperature);
    } else {
      drawDebugMonitor((int)latestEmgPercent, latestEmgSignal, EMG_Rest, EMG_Max, globalVariance, globalThreshold, globalMotion, currentTemperature);
    }

    lastUpdate = now;
  }

  uploadSensorReading();
}
