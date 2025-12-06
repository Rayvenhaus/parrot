# Parrot – **P**eople’s **AR**duino Radiation m**O**ni**T**or

Parrot is a small, network-connected Geiger counter node built on an **Arduino Leonardo + W5500 Ethernet (PoE) shield**.  
It continuously measures radiation (CPM) and reports to [radmon.org](https://radmon.org/)**, while also sending a
rich telemetry heartbeat to a self-hosted **Parrot HQ** web backend.

`*PARROT warns the people!  By the peoples, of the peoples, for the peoples!  Viva la monitoreo!!!`
> Originally forked from [rjelbert/geiger_counter](https://github.com/rjelbert/geiger_counter/tree/main/arduino_jelbert_geiger_counter)** and now heavily extended and refactored into a full remote monitor.

---

## Features

- 📡 **Geiger counter logging**
  - Counts pulses from a Geiger tube on digital pin **2 (INT0)**.
  - Computes **CPM** and derived **µSv/h** using a configurable conversion factor.
  - Protects against runaway readings with sensible upper bounds.

- 🌡️ **Interior environment monitoring**
  - **DHT11** sensor on digital pin **4** for internal **temperature (°C)** and **humidity (%)**.
  - High-temperature threshold (`HIGH_TEMP_THRESHOLD_C`, default 50°C) marks the node as WARN.

- 🟢🟡🔴 **RGB system status LED**
  - Common-cathode RGB LED on pins **5 (R), 6 (G), 7 (B)**.
  - Status mapping:
    - **GREEN** – sensor OK, network OK, last radmon.org upload succeeded.
    - **YELLOW** – network warning (radmon issues) or high internal temperature.
    - **RED** – sensor fault (no pulses for multiple consecutive logging periods).

- 🌐 **radmon.org upload**
  - Periodic HTTP GET to submit CPM readings to radmon.org.
  - Proper HTTP status parsing:
    - Treats `200` and `204` as success.
    - All other codes flagged as failures and logged for debugging.

- 🏠 **Parrot HQ heartbeat (self-hosted backend)**
  - Optional HTTP GET to `https://www.myndworx.com/parrot/api/status.php` (or your own host).
  - Sends:
    - Device ID (`DEVICE_ID`)
    - Shared secret (`API_SECRET`)
    - Firmware version
    - Uptime (seconds)
    - CPM and derived radiation band (LOW / NORM / WARN / DANGER)
    - Last radmon.org HTTP status code
    - Interior temperature and humidity
    - Zero-CPM streak count
    - Last fail reason
    - Reset cause flags (from `MCUSR`)
  - Designed to feed a simple dashboard + time-series logging database.

- 🧠 **Robustness and diagnostics**
  - Hardware watchdog (WDT) enabled at 8 seconds.
  - Captures reset cause (`PORF`, `BORF`, `EXTRF`, `WDRF`) at early startup.
  - Serial output logs CPM, µSv/h, temp, humidity, rad state, HTTP codes, and errors.

---

## Hardware Overview

### Core platform

- **Arduino Leonardo** (ATmega32u4)
- **W5500 Ethernet (PoE) shield** or PoE-enabled W5500 carrier
- **Geiger counter kit** with pulse output to 5V logic (e.g. SBM-20/J305 based kit)

### Sensors & indicators

- **Geiger tube pulse output → D2**
  - Interrupt-driven counting on `INT0` (digital pin 2).
- **DHT11 temp/humidity → D4**
  - 5V supply, data pin with suitable pull-up (usually integrated in modules).
- **RGB system status LED**
  - Common-cathode LED with appropriate series resistors:
    - **R** → D5
    - **G** → D6
    - **B** → D7

### Power

- 5V via PoE (through your W5500 PoE hardware) **or** a stable external 5V supply.
- Ensure the Geiger high-voltage section is properly isolated and respects all safety requirements.

---

## Wiring Summary

| Function              | Arduino Pin | Notes                                       |
|-----------------------|------------:|---------------------------------------------|
| Geiger pulse (INT0)   | D2          | From Geiger kit pulse output (5V logic)     |
| DHT11 data            | D4          | Use DHT11 module with VCC/GND + data        |
| RGB LED – Red         | D5          | With series resistor, common cathode to GND |
| RGB LED – Green       | D6          | With series resistor                        |
| RGB LED – Blue        | D7          | With series resistor                        |
| W5500 CS              | D10         | `SS` / chip select                          |
| W5500 RST             | D3          | Reset line to W5500                         |

---

## Firmware Configuration

Open the sketch and adjust the configuration section near the top:

```cpp
#define FW_NAME     "Parrot"
#define FW_VERSION  "v1.4.0"

#define DEVICE_ID   "REPLACE_WITH_YOUR_PARROT_ID"
#define API_SECRET  "REPLACE_WITH_YOUR_SECRET"

#define STATUS_PING_ENABLE  1
#define STATUS_HOST         "www.myndworx.com"
#define STATUS_PORT         80
#define STATUS_PATH         "/parrot/api/status.php"
```

### 1. Device identity

- `DEVICE_ID`  
  A short, unique ID for the node, e.g. `"PARROT-001"`, `"PARROT-ROOF"`, etc.  
  Displayed in Parrot HQ and used as the primary key in the backend database.

### 2. Parrot HQ secret

- `API_SECRET`  
  Shared secret between the node and Parrot HQ.  
  Must match `PARROT_API_SECRET` in your HQ `config.php`.  
  This is **not strong cryptography**, just a basic gate to keep random noise out.

### 3. Parrot HQ endpoint

- `STATUS_HOST`, `STATUS_PORT`, `STATUS_PATH`  
  Point these at your own backend if you are not using `myndworx.com`.  
  The firmware assumes HTTP; if you terminate TLS at a reverse proxy, configure that at the server side.

### 4. radmon.org credentials

Inside the radmon upload block, set your username and password:

```cpp
client.print(F("GET /radmon.php?function=submit&user=YOUR_USERNAME&password=YOUR_PASSWORD&value="));
```

Replace `YOUR_USERNAME` and `YOUR_PASSWORD` with your actual radmon.org credentials.

---

## Radiation Thresholds

The firmware classifies CPM into bands using these defines:

```cpp
#define CPM_NORMAL_MIN       20UL
#define CPM_NORMAL_MAX       40UL
#define CPM_WARN_THRESHOLD   100UL
#define CPM_DANGER_THRESHOLD 275UL
```

- **LOW**    – everything below `CPM_NORMAL_MIN`
- **NORMAL** – between `CPM_NORMAL_MIN` and `CPM_NORMAL_MAX`
- **WARN**   – ≥ `CPM_WARN_THRESHOLD`
- **DANGER** – ≥ `CPM_DANGER_THRESHOLD`

These bands are used only for **status signalling and Parrot HQ telemetry**.  
Actual safety assessment should be based on your tube’s calibration and local regulations.

---

## Build & Flash

1. **Board selection**

   - Select **Arduino Leonardo** in the Arduino IDE (or equivalent in PlatformIO).
   - Make sure you are targeting the correct serial port.

2. **Required libraries**

   - `Ethernet` (built-in or official Arduino Ethernet/W5500 library)
   - `DHT sensor library` (Adafruit fork or compatible)

3. **Compile & upload**

   - Open the sketch.
   - Configure `DEVICE_ID`, `API_SECRET`, and radmon.org credentials.
   - Compile and upload to the Leonardo.

4. **Serial monitor**

   - Set 9600 baud.
   - On boot you should see:
     - Firmware build string (`Parrot v1.3.2 (DATE TIME)`).
     - Basic startup info.
     - Periodic logs of CPM, µSv/h, temperature, humidity, and upload results.

---

## Parrot HQ (Backend)

Parrot HQ is a small PHP/MySQL application that:

- Accepts GET requests at `/parrot/api/status.php` with the fields the firmware sends.
- Stores:
  - Devices in a `parrot_devices` table.
  - Time-series heartbeats in a `parrot_heartbeats` table.
- Renders a simple dashboard at `/parrot/`:
  - Cards per device with:
    - Online/offline state (based on last-seen time).
    - Radiation band (LOW/NORM/WARN/DANGER).
    - Latest CPM, temperature, humidity.
    - Last HTTP code from radmon.org and last fail reason.

This README focuses on the firmware side; backend setup is documented separately in the Parrot HQ docs.

---

## Safety Notes

- Geiger counter kits involve **high voltage** internally; only use reputable designs and follow all safety guidance from the kit manufacturer.
- This firmware is intended for **hobbyist monitoring and experimentation**, not for certified radiation safety systems.
- Always cross-check readings with calibrated instruments where safety-critical decisions are involved.

---

## License & Attribution

- **License:** MIT – see `LICENSE` in this repository.
- Original project: [`rjelbert/geiger_counter`](https://github.com/rjelbert/geiger_counter) (MIT).
- Parrot firmware:
  - Renames and extends the original sketch.
  - Adds Ethernet/W5500 support, DHT11 environment telemetry, RGB status LED, watchdog, Parrot HQ heartbeat, and various robustness improvements.

If you publish derivative work or integrate Parrot into a larger system, please keep:

- The original attribution to JiangJie Zhang and `rjelbert/geiger_counter`.
- The MIT license terms and copyright notices.

---

Happy counting, and may your Parrots quietly sing of low background levels. 🦜
