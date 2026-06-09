// Smart Bicycle Fitness Tracker — ESP32 Firmware
// ME2400 | IIT Madras, Dept. of Mechanical Engineering
//
// Pin Map:
//   GPIO 34 — Hall sensor (input-only)
//   GPIO 21 — I2C SDA  (LCD 0x27 + MAX30102 0x57)
//   GPIO 22 — I2C SCL
//   GPIO 25 — Buzzer (OUTPUT)
//   VIN     — 5V  → LCD VCC, Buzzer VCC
//   3V3     — 3.3V → MAX30102 VCC, Hall VCC
//   GND     — Common ground

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// BLE UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CONFIG_CHAR_UUID    "d1e2f3a4-b5c6-7890-abcd-ef1234567890"

// Pins
#define HALL_PIN   34
#define BUZZER_PIN 25

// Hardware objects
LiquidCrystal_I2C lcd(0x27, 16, 2);
MAX30105 particleSensor;
Preferences prefs;

// BLE
BLEServer*         pServer         = NULL;
BLECharacteristic* pCharacteristic = NULL;
BLECharacteristic* pConfigChar     = NULL;
bool deviceConnected = false;

// Rider config (loaded from NVS; updated via BLE write)
float wheelCircumference = 2.0106f;  // metres (default: 700c road tyre)
float riderWeight        = 70.0f;    // kg
const int BPM_ALERT_THRESHOLD = 180;

// MET lookup by speed
float getMET(float speedKmh) {
  if (speedKmh < 16.0f) return 4.0f;
  if (speedKmh < 20.0f) return 6.0f;
  if (speedKmh < 25.0f) return 8.0f;
  return 10.0f;
}

// Screen rotation
const unsigned long SCREEN_INTERVAL_MS = 5000;
int           currentScreen      = 0;
unsigned long lastScreenChangeMs = 0;

// Speed & distance
volatile int  pulseCount      = 0;
float         speedKmh        = 0.0f;
float         totalDistanceKm = 0.0f;

// Heart rate
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot       = 0;
long  lastBeat       = 0;
float beatsPerMinute = 0.0f;
int   beatAvg        = 0;

// Derived metrics
float averageSpeedKmh = 0.0f;
float caloriesBurned  = 0.0f;

// Alert
bool heartRateAlertActive = false;

// Timers
unsigned long rideStartTime   = 0;
unsigned long lastSpeedCalcMs = 0;
unsigned long lastDisplayMs   = 0;
unsigned long lastBLEMs       = 0;
unsigned long lastBPMPollMs   = 0;

// Pad/truncate string to exactly 'width' characters
String padRight(String s, int width) {
  while ((int)s.length() < width) s += ' ';
  if   ((int)s.length() > width)  s  = s.substring(0, width);
  return s;
}

// ISR — increments pulse counter on each Hall sensor falling edge
void IRAM_ATTR onHallPulse() {
  pulseCount++;
}

// Parse "radius:X,weight:Y" from BLE and persist to NVS
void parseAndSaveConfig(String payload) {
  int rIdx = payload.indexOf("radius:");
  if (rIdx != -1) {
    int numStart = rIdx + 7;
    int commaIdx = payload.indexOf(',', numStart);
    String rStr  = (commaIdx != -1) ? payload.substring(numStart, commaIdx)
                                    : payload.substring(numStart);
    float r = rStr.toFloat();
    if (r > 0.25f && r < 0.40f) {
      wheelCircumference = 2.0f * 3.14159265f * r;
      prefs.putFloat("circumf", wheelCircumference);
      Serial.printf("[Config] Radius %.4f m → circumference %.4f m\n", r, wheelCircumference);
    }
  }

  int wIdx = payload.indexOf("weight:");
  if (wIdx != -1) {
    String wStr = payload.substring(wIdx + 7);
    float w = wStr.toFloat();
    if (w > 30.0f && w < 200.0f) {
      riderWeight = w;
      prefs.putFloat("weight", riderWeight);
      Serial.printf("[Config] Weight saved: %.1f kg\n", riderWeight);
    }
  }

  String ack = "ACK radius:" + String(wheelCircumference / (2.0f * 3.14159265f), 4)
             + ",weight:"    + String(riderWeight, 1);
  pCharacteristic->setValue(ack.c_str());
  pCharacteristic->notify();
}

// BLE connection callbacks
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Phone connected.");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Disconnected. Restarting advertising...");
    BLEDevice::startAdvertising();
  }
};

// BLE config write callback
class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String payload = String(pChar->getValue().c_str());
    Serial.print("[BLE] Config received: "); Serial.println(payload);
    parseAndSaveConfig(payload);
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("=== Bike Fitness Tracker Booting ===");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  attachInterrupt(digitalPinToInterrupt(HALL_PIN), onHallPulse, FALLING);

  prefs.begin("bikeconfig", false);
  wheelCircumference = prefs.getFloat("circumf", 2.0106f);
  riderWeight        = prefs.getFloat("weight",  70.0f);
  Serial.printf("[Config] circumference: %.4f m | weight: %.1f kg\n",
                wheelCircumference, riderWeight);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Fitness Tracker");
  lcd.setCursor(0, 1); lcd.print("Loading...");
  delay(2000);
  lcd.clear();
  Serial.println("[LCD] Ready at 0x27.");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[ERROR] MAX30102 not found.");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("MAX30102 ERROR");
    lcd.setCursor(0, 1); lcd.print("Check wiring!");
    while (1) { delay(100); }
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);
  Serial.println("[Sensor] MAX30102 ready at 0x57.");

  BLEDevice::init("BikeFitnessTracker");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());

  pConfigChar = pService->createCharacteristic(
    CONFIG_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pConfigChar->setCallbacks(new ConfigCallbacks());

  pService->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as 'BikeFitnessTracker'.");

  unsigned long startMs = millis();
  rideStartTime      = startMs;
  lastSpeedCalcMs    = startMs;
  lastScreenChangeMs = startMs;
  lastBPMPollMs      = startMs;

  Serial.println("=== Ready. Screens rotate every 5s. ===");
}

// Poll MAX30102 FIFO and update beatAvg
int readBPM() {
  particleSensor.check();

  while (particleSensor.available()) {
    long irValue = particleSensor.getFIFOIR();

    if (irValue > 50000) {
      if (checkForBeat(irValue)) {
        long delta     = millis() - lastBeat;
        lastBeat       = millis();
        beatsPerMinute = 60.0f / (delta / 1000.0f);

        if (beatsPerMinute > 20 && beatsPerMinute < 255) {
          rates[rateSpot++] = (byte)beatsPerMinute;
          rateSpot %= RATE_SIZE;

          beatAvg = 0;
          for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
          beatAvg /= RATE_SIZE;
        }
      }
    }
    particleSensor.nextSample();
  }
  return beatAvg;
}

// Format elapsed ride time as "HH:MM:SS"
String getRideTimeString() {
  unsigned long totalSec = (millis() - rideStartTime) / 1000;
  int h = totalSec / 3600;
  int m = (totalSec % 3600) / 60;
  int s = totalSec % 60;
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

// Render the active screen onto the 16x2 LCD
void updateLCD(float speed, float distance, String rideTime,
               int bpm, bool alert, float avgSpeed, float cals) {

  if (currentScreen == 0) {
    String line1 = "Speed:" + String(speed, 1) + " km/h";
    String line2 = "D:" + String(distance, 2) + "km " + rideTime.substring(3, 8);
    lcd.setCursor(0, 0); lcd.print(padRight(line1, 16));
    lcd.setCursor(0, 1); lcd.print(padRight(line2, 16));

  } else if (currentScreen == 1) {
    String line1 = "HR: " + String(bpm) + " BPM";
    lcd.setCursor(0, 0); lcd.print(padRight(line1, 16));
    lcd.setCursor(0, 1); lcd.print(alert ? "ALERT: HIGH!    "
                                         : "ALERT: --       ");
  } else {
    String line1 = "AVG:" + String(avgSpeed, 1) + " km/h";
    String line2 = "CAL:" + String((int)cals)   + " kcal";
    lcd.setCursor(0, 0); lcd.print(padRight(line1, 16));
    lcd.setCursor(0, 1); lcd.print(padRight(line2, 16));
  }
}

// Advance currentScreen every SCREEN_INTERVAL_MS
void handleScreenRotation(unsigned long now) {
  if (now - lastScreenChangeMs >= SCREEN_INTERVAL_MS) {
    currentScreen      = (currentScreen + 1) % 3;
    lastScreenChangeMs = now;
  }
}

// Push CSV telemetry packet to connected phone via BLE NOTIFY
void sendBLEPacket(float speed, float distance, String rideTime,
                   int bpm, float cals) {
  if (!deviceConnected) return;

  String data = "Speed:"           + String(speed, 1)    +
                ",Distance:"       + String(distance, 3) +
                ",Time:"           + rideTime             +
                ",Heart rate:"     + String(bpm)          +
                ",Calories Burnt:" + String((int)cals);

  pCharacteristic->setValue(data.c_str());
  pCharacteristic->notify();
  Serial.print("[BLE] Sent: "); Serial.println(data);
}

// Main loop — non-blocking, millis()-based task scheduling
// Task schedule:
//   Every 20 ms   — poll MAX30102 FIFO
//   Every 1000 ms — compute speed, distance, calories, check BPM alert
//   Every loop    — check screen rotation (fires every 5 s)
//   Every 500 ms  — refresh LCD + send BLE packet
void loop() {
  unsigned long now = millis();

  if (now - lastBPMPollMs >= 20) {
    readBPM();
    lastBPMPollMs = now;
  }

  if (now - lastSpeedCalcMs >= 1000) {
    unsigned long elapsed = now - lastSpeedCalcMs;

    noInterrupts();
    int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float distM = pulses * wheelCircumference;
    speedKmh         = (elapsed > 0) ? (distM * 3600.0f / (float)elapsed) : 0.0f;
    totalDistanceKm += distM / 1000.0f;

    float rideTimeHours = (now - rideStartTime) / 3600000.0f;
    averageSpeedKmh = (rideTimeHours > 0.0f) ? (totalDistanceKm / rideTimeHours) : 0.0f;

    caloriesBurned += getMET(speedKmh) * riderWeight * (1.0f / 3600.0f);

    heartRateAlertActive = (beatAvg > BPM_ALERT_THRESHOLD);
    digitalWrite(BUZZER_PIN, heartRateAlertActive ? HIGH : LOW);

    Serial.printf("[DATA] SPD:%.1f | DST:%.3fkm | BPM:%d | AVG:%.1f | CAL:%.0f | Alert:%s\n",
      speedKmh, totalDistanceKm, beatAvg, averageSpeedKmh, caloriesBurned,
      heartRateAlertActive ? "YES" : "no");

    lastSpeedCalcMs = now;
  }

  handleScreenRotation(now);

  if (now - lastDisplayMs >= 500) {
    String rideTime = getRideTimeString();
    updateLCD(speedKmh, totalDistanceKm, rideTime,
              beatAvg, heartRateAlertActive, averageSpeedKmh, caloriesBurned);
    sendBLEPacket(speedKmh, totalDistanceKm, rideTime, beatAvg, caloriesBurned);
    lastDisplayMs = now;
    lastBLEMs     = now;
  }
}
