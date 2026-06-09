# 🚴 Smart Bicycle Fitness Tracker

A compact, handlebar-mounted cycling performance monitor built on the ESP32. Measures speed, distance, heart rate, and calories burned in real time — displayed on a 16×2 LCD and streamed live to a smartphone over BLE.

---

## Overview

The tracker monitors six real-time cycling metrics, displays them on a handlebar-mounted 16×2 LCD (across three auto-rotating screens), and streams live data to a smartphone over Bluetooth Low Energy (BLE) every 500 ms.

| Metric | Source |
|--------|--------|
| Instantaneous Speed (km/h) | Hall-effect sensor + wheel circumference |
| Cumulative Distance (km) | Pulse integration |
| Ride Duration (HH:MM:SS) | Elapsed time |
| Heart Rate (BPM) | MAX30102 PPG sensor |
| Average Speed (km/h) | Distance ÷ ride time |
| Calories Burned (kcal) | MET model |

---

## Hardware

| Component | Role | Interface |
|-----------|------|-----------|
| ESP32 NodeMCU-32S (WROOM-32) | Microcontroller | — |
| MAX30102 | Heart rate (PPG) | I²C @ 0x57 |
| LM393 Hall-effect module | Wheel speed | GPIO 34 (ISR) |
| 16×2 LCD + PCF8574 backpack | Display | I²C @ 0x27 |
| Active buzzer module | High-HR alert | GPIO 25 |
| USB power bank (5 V) | Power supply | VIN |

### Pin Map

```
GPIO 21 — I²C SDA  (LCD + MAX30102, shared bus)
GPIO 22 — I²C SCL  (LCD + MAX30102, shared bus)
GPIO 34 — Hall sensor D0 (input-only, falling-edge ISR)
GPIO 25 — Buzzer I/O (OUTPUT)
VIN     — 5 V  → LCD VCC + Buzzer VCC
3V3     — 3.3 V → MAX30102 VCC + Hall VCC
GND     — Common ground
```

---

## Mathematical Model

### Speed and Distance
$$v \,[\text{km/h}] = \frac{N \times C_{\text{wheel}}}{\Delta t[\text{s}]} \times 3.6 \qquad \Delta D\,[\text{km}] = \frac{N \times C_{\text{wheel}}}{1000}$$

- $N$ = Hall pulses in interval; $C_{\text{wheel}} = 2.0106\,\text{m}$ (700c default, configurable)
- $\Delta t$ = actual measured elapsed time (corrects for loop jitter)

### Average Speed and Calories (MET Model)
$$\bar{v} = \frac{D_{\text{total}}}{T_{\text{ride}}} \qquad \Delta\text{kcal} = \text{MET}(v) \times m_{\text{rider}} \times \frac{1}{3600}$$

| Speed Range | MET |
|-------------|-----|
| < 16 km/h   | 4.0 (Easy/Leisure) |
| 16–20 km/h  | 6.0 (Moderate) |
| 20–25 km/h  | 8.0 (Vigorous) |
| ≥ 25 km/h   | 10.0 (Racing) |

### Heart Rate (BPM)
$$\text{BPM}_i = \frac{60{,}000}{\tau_i[\text{ms}]} \qquad \overline{\text{BPM}} = \frac{1}{4}\sum_{k=0}^{3} \text{rates}[k]$$

A 4-element ring buffer smooths erratic single-beat readings.

---

## Firmware Architecture

The firmware is **non-blocking** — `delay()` is never called in `loop()`. Each task runs on its own `millis()` timestamp.

| Task | Interval | Mechanism |
|------|----------|-----------|
| MAX30102 FIFO poll | 20 ms | `lastBPMPollMs` |
| Speed / metrics calc | 1000 ms | `lastSpeedCalcMs` |
| Screen auto-rotation | 5000 ms | `lastScreenChangeMs` |
| LCD refresh + BLE send | 500 ms | `lastDisplayMs` |
| Hall-sensor ISR | Async | Hardware interrupt, GPIO 34 FALLING |

### LCD Screens (auto-rotate every 5 s)

| Screen | Row 1 | Row 2 |
|--------|-------|-------|
| Ride View | `Speed:23.4 km/h` | `D:5.12km 14:32` |
| Health View | `HR: 142 BPM` | `ALERT: HIGH!` or `ALERT: --` |
| Summary View | `AVG:18.3 km/h` | `CAL:180 kcal` |

### BLE Interface

| Item | Value |
|------|-------|
| Advertised name | `BikeFitnessTracker` |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Data characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Config characteristic UUID | `d1e2f3a4-b5c6-7890-abcd-ef1234567890` |
| Data format | `Speed:X,Distance:X,Time:X,Heart rate:X,Calories Burnt:X` |
| Update rate | 500 ms (NOTIFY) |

**Configuring from phone:** Write `radius:<metres>,weight:<kg>` to the config characteristic. Values are validated and persisted to NVS flash across reboots.

---

## Dependencies

Install via Arduino IDE Library Manager:

- `LiquidCrystal_I2C` — Frank de Brabander
- `SparkFun MAX3010x Pulse and Proximity Sensor Library` — SparkFun
- `ESP32 BLE Arduino` — Neil Kolban (bundled with Espressif core)
- `Preferences` — bundled with Espressif ESP32 Arduino core

**Board:** ESP32 Dev Module  
**Core:** [Espressif ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)

---

## Setup

1. Clone the repo and open `firmware/main.ino` in the Arduino IDE.
2. Install the dependencies listed above.
3. Select **Tools → Board → ESP32 Dev Module**.
4. Wire components as per the pin map above.
5. Upload. Open Serial Monitor at **115200 baud**.
6. On first boot, defaults are used (`C_wheel = 2.0106 m`, `weight = 70 kg`). Update via BLE write.

---

## Alert

When the averaged BPM exceeds **180 BPM** (configurable via `BPM_ALERT_THRESHOLD`), the buzzer activates and the LCD Health View shows `ALERT: HIGH!`. The BLE telemetry packet also reflects the alert state.

---

## Repository Structure

```
smart-bicycle-fitness-tracker/
├── firmware/
│   └── main.ino          # ESP32 Arduino firmware
├── docs/
│   └── report.pdf        # Full project report (ME2400)
├── .gitignore
└── README.md
```

---

