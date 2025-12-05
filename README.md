# **PARROT** – **P**eople’s **AR**duino **R**adiation m**O**ni**T**or
*(Arduino Leonardo / W5500 / OLED / DHT11)*
# Parrot — a faithful, noisy little sentinel watching for radiation and raising hell when things get dangerous. #

## Overview
### Firmware Version: **v1.3.0**
This firmware runs on an Arduino Leonardo equipped with:

- Geiger counter kit (VIN → Pin 2 interrupt)
- W5500 Ethernet (PoE or splitter)
- RGB system LED
- OLED display (SSD1306, 128x64)
- DHT11 interior temperature / humidity sensor
- Optional telemetry endpoint (future use)

It uploads Counts Per Minute (CPM) to **radmon.org** at set intervals, and 
provides local diagnostics for service and maintenance.

---

## Name & Origin

**Parrot** is the *People’s ARduino Radiation mOniTor* — a noisy little sentinel
that watches for radiation and squawks when something looks wrong.

This firmware began life as a derivative of the
[`rjelbert/geiger_counter`](https://github.com/rjelbert/geiger_counter) project,
and has since been extensively rewritten and expanded into a standalone system.

The project retains attribution to the original author and is released under
the MIT License.

## Features

### Geiger Counter
- Interrupt-driven counts
- CPM calculated at fixed period
- Protection against runaway readings
- Radiation safety bands:
  - **LOW**
  - **NORMAL** (20–40 CPM)
  - **WARNING** (≥100 CPM)
  - **DANGER** (≥275 CPM)

### System Health
- Hardware watchdog (8 seconds)
- Brown-out and reset-cause reporting
- Zero-CPM detection
  - 3 consecutive 0-CPM cycles = sensor error

### Ethernet
- W5500-based
- DHCP, link status, and hardware checks
- Upload to radmon.org:
  - HTTP 200/204 = success
  - Any other code = warning

### Display
- **OLED OFF by default**
- **Button toggle**
- Displays:
  - Firmware version
  - Status summary
  - CPM, uptime, temp, humidity
  - Last upload result
  - Last successful upload age
  - Fail reason and HTTP code
  - Local IP address

### Service Mode
- Hold display button during boot:
  - Shorter log period (10s)
  - OLED forced ON
  - **No upload to radmon**
  - Ideal for field maintenance

### Telemetry (future)
- Stub for `/status?id=GM-001&...`
- Disabled by default:

  #define STATUS_PING_ENABLE 0

---

## Wiring Summary

| Function              | Pin |
|----------------------|----:|
| Geiger INT input     | **2** |
| Ethernet Reset       | **3** |
| System LED RGB       | **5, 6, 7** |
| Display Button       | **8** |
| DHT11 Temp/Humidity  | **4** |
| OLED (I²C)           | **SCL/SDA** |
| W5500 Ethernet       | **SPI (SS pin 10)** |

---

## Operation

### Normal Mode
- LED shows system health:
- **Green**: OK (network + sensor + temperature)
- **Yellow**: warning (HTTP, high temp)
- **Red**: error (sensor)

### Service Mode
Hold the button during boot:

- OLED ON
- Short log period (10 seconds)
- No upload
- Ideal for troubleshooting on a ladder

---

## Environmental Notes

- DHT11 monitors interior temperature/humidity
- High-temperature warning if ≥50°C
- Recommend:
- Desiccant pack
- Gore vent / breathable membrane
- Conformal coat for PCB if harsh weather

---

## Deployment Notes

- One cable via **PoE** is recommended
- Inside a weather-proof enclosure
- Display normally OFF
- On-demand diagnostics with button

---

## License

Parrot is released under the **MIT License**.

This repository includes a `LICENSE` file (MIT License text) and a `NOTICE` file
documenting the use of code originally derived from:

- `rjelbert/geiger_counter` (MIT License) – JiangJie Zhang

No endorsement by the original author is implied.
