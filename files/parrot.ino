/*
 * -------------------------------------------------------------
 *  Parrot Prime Firmware v1.5.1 (SafeBoot)
 *  Model: Parrot Prime (Leonardo + DFRobot PoE Shield)
 *  Author: Myndworx Asylum - Steven Sheeley
 *  Copyright &copy 2025 = MwA & Steven Sheeley
 *  License: MIT
 * -------------------------------------------------------------
 */

#include <SPI.h>
#include <Ethernet.h>
#include <DHT.h>
#include <string.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <avr/io.h>

// ----------------- Configuration -----------------
//Enable/Disable Debugging display
#define DEBUG_SERIAL 1

#define FW_NAME      "Parrot Radiation Monitor - "
#define FW_VERSION   "v1.5.1 SafeBoot"
#define SERIAL_NUM   "ENTER_DEVICE_SERIAL_NUM"
#define FW_BUILD     FW_NAME" "FW_VERSION - " (" __DATE__ " " __TIME__ ")"

#define STATUS_PING_ENABLE 1
#define STATUS_HOST "www.myndworx.com"
#define STATUS_PORT 80
#define STATUS_PATH "/parrot/api/status.php"

#define DEVICE_ID   "REPLACE_WITH_YOUR_ID"
#define API_SECRET  "REPLACE_WITH_YOUR_SECRET"
#define RADMON_USER "REPLACE_WITH_YOUR_USERNAME"
#define RADMON_PASS "REPLACE_WITH_YOUR_PASSWORD"

// ----------------- Radiation Safety -----------------
#define CPM_NORMAL_MIN        20UL
#define CPM_NORMAL_MAX        40UL
#define CPM_WARN_THRESHOLD    100UL
#define CPM_DANGER_THRESHOLD  275UL
#define CPM_MAX_VALID         50000UL

enum RadState : uint8_t {
  RAD_LOW = 0, RAD_NORMAL = 1, RAD_WARN = 2, RAD_DANGER = 3
};

// ----------------- DHT Sensor -----------------
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
#define TEMP_WARN_THRESHOLD_C 50.0f
#define TEMP_HIGH_THRESHOLD_C 60.0f

// ----------------- Ethernet -----------------
const byte mac[] = { 0x2E, 0x3D, 0x4E, 0x5F, 0x6E, 0x7D };
const char server[] = "radmon.org";
EthernetClient client;
EthernetClient statusClient;

// ----------------- Timing -----------------
#define LOG_PERIOD 60000UL
#define MAX_PERIOD 60000UL
#define CONV_FACTOR 0.00812037f
#define RADMON_FAIL_TIMEOUT_MS (15UL * 60UL * 1000UL)

// Leonardo + W5100 Reset lines
#define SS 10
#define RST 3

// ----------------- RGB LED -----------------
#define LED_R 5
#define LED_G 6
#define LED_B 7

void setStatusLED(byte r, byte g, byte b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

byte ledR = LOW, ledG = LOW, ledB = LOW;
bool ledFlash = false, ledOn = true;
unsigned long ledLastToggle = 0;
const unsigned long LED_FLASH_PERIOD = 500UL;

void applyLed() {
  if (ledOn) setStatusLED(ledR, ledG, ledB);
  else setStatusLED(LOW, LOW, LOW);
}
void setLedState(byte r, byte g, byte b, bool flash) {
  ledR = r; ledG = g; ledB = b;
  ledFlash = flash; ledOn = true;
  ledLastToggle = millis();
  applyLed();
}
void ledTick() {
  if (!ledFlash) return;
  unsigned long now = millis();
  if (now - ledLastToggle >= LED_FLASH_PERIOD) {
    ledLastToggle = now;
    ledOn = !ledOn;
    applyLed();
  }
}

// ----------------- Fail Reasons -----------------
enum FailReason : uint8_t { FAIL_NONE=0, FAIL_CONNECT=1, FAIL_NO_OK=2, FAIL_DHCP=3 };

// ----------------- Globals -----------------
volatile unsigned long counts;
unsigned long cpm;
unsigned int multiplier;
unsigned long previousMillis;
float usvh;
byte zeroCount;
unsigned long lastSuccessMillis;
unsigned long bootMillis;
FailReason lastFailReason;
int lastHttpStatusCode;
int hqHttpStatusCode;
float interiorTempC;
float interiorHum;
bool netReady = true;
uint8_t resetCause __attribute__((section(".noinit")));

#define ZERO_CPM_FAULT_COUNT 3

// ----------------- Reset Cause -----------------
void getResetCause(void) __attribute__((naked)) __attribute__((section(".init3")));
void getResetCause(void) {
  resetCause = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

// ----------------- ISR -----------------
void tube_impulse() { counts++; }

// ----------------- Radiation state -----------------
RadState getRadiationState(unsigned long cpmValue) {
  if (cpmValue >= CPM_DANGER_THRESHOLD) return RAD_DANGER;
  else if (cpmValue >= CPM_WARN_THRESHOLD) return RAD_WARN;
  else if (cpmValue >= CPM_NORMAL_MIN && cpmValue <= CPM_NORMAL_MAX) return RAD_NORMAL;
  else return RAD_LOW;
}

// ----------------- Health Model -----------------
struct DeviceHealth {
  uint8_t level;
  RadState radState;
  bool sensorFault;
  bool tempWarn;
  bool tempHigh;
  bool netOK;
};
DeviceHealth health;

void updateHealthModel() {
  health.radState = getRadiationState(cpm);
  health.sensorFault = (zeroCount >= ZERO_CPM_FAULT_COUNT);
  health.tempWarn = (interiorTempC >= TEMP_WARN_THRESHOLD_C && interiorTempC < TEMP_HIGH_THRESHOLD_C);
  health.tempHigh = (interiorTempC >= TEMP_HIGH_THRESHOLD_C);

  bool netOKLocal;
  if (lastFailReason == FAIL_DHCP) netOKLocal = false;
  else if (lastSuccessMillis == 0) netOKLocal = false;
  else {
    unsigned long now = millis();
    netOKLocal = (now - lastSuccessMillis <= RADMON_FAIL_TIMEOUT_MS);
  }
  health.netOK = netOKLocal;

  uint8_t lvl = 0;
  if (!health.netOK) lvl = max(lvl, 1);
  if (health.sensorFault || health.tempWarn || health.radState == RAD_WARN) lvl = max(lvl, 2);
  if (health.tempHigh || health.radState == RAD_DANGER) lvl = max(lvl, 3);
  health.level = lvl;
}

// ----------------- LED Update -----------------
void updateStatusLED() {
  switch (health.level) {
    case 3: setLedState(HIGH, LOW, LOW, true); break;   // Flashing red
    case 2: setLedState(HIGH, LOW, LOW, false); break;  // Solid red
    case 1: setLedState(HIGH, HIGH, LOW, false); break; // Yellow
    default:setLedState(LOW, HIGH, LOW, false); break;  // Green
  }
}

// ----------------- DHCP SafeBoot Logic -----------------
bool attemptDHCP() {
  int retries = 3;
  while (retries-- > 0) {
    if (Ethernet.begin(mac) != 0) return true;
    #if DEBUG_SERIAL
    Serial.println(F("[ETH] DHCP failed, retrying in 5s..."));
    #endif
    for (int i = 0; i < 10; i++) {
      wdt_reset();
      delay(500);
    }
  }
  return false;
}

// ----------------- Setup -----------------
void setup() {
  counts = 0; cpm = 0; multiplier = MAX_PERIOD / LOG_PERIOD;
  previousMillis = millis(); bootMillis = millis();
  lastFailReason = FAIL_NONE; lastHttpStatusCode = 0;
  hqHttpStatusCode = 0; interiorTempC = -127.0f;
  interiorHum = -1.0f; zeroCount = 0;
  lastSuccessMillis = 0; netReady = true;

  Serial.begin(9600);
  #if DEBUG_SERIAL
    unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart < 3000UL)) { ; }
  #endif
  Serial.println(); Serial.println(FW_BUILD);

  attachInterrupt(digitalPinToInterrupt(2), tube_impulse, FALLING);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLedState(LOW, HIGH, LOW, false);
  dht.begin();

  // PoE shield reset sequence
  pinMode(SS, OUTPUT); pinMode(RST, OUTPUT);
  digitalWrite(SS, HIGH); digitalWrite(RST, HIGH);
  delay(200); digitalWrite(RST, LOW);
  delay(200); digitalWrite(RST, HIGH);
  delay(200); digitalWrite(SS, LOW);
  Ethernet.init(10);
  delay(1000);

  // SafeBoot DHCP logic
  if (attemptDHCP()) {
    netReady = true;
    lastFailReason = FAIL_NONE;
    #if DEBUG_SERIAL
    Serial.print(F("[ETH] DHCP OK. IP: "));
    Serial.println(Ethernet.localIP());
    #endif
  } else {
    netReady = false;
    lastFailReason = FAIL_DHCP;
    #if DEBUG_SERIAL
    Serial.println(F("[ETH] DHCP failed after retries. Running offline."));
    #endif
  }

  wdt_enable(WDTO_8S);
  updateHealthModel();
  updateStatusLED();

  #if DEBUG_SERIAL
  Serial.println(F("[BOOT] System initialized. SafeBoot active."));
  #endif
}

// ----------------- Main Loop -----------------
void loop() {
  wdt_reset();
  Ethernet.maintain();
  static unsigned long lastDhcpRetry = 0;

  // Periodic DHCP retry if offline
  if (!netReady && (millis() - lastDhcpRetry >= 600000UL)) {
    lastDhcpRetry = millis();
    if (attemptDHCP()) {
      netReady = true;
      lastFailReason = FAIL_NONE;
      lastSuccessMillis = millis();
      #if DEBUG_SERIAL
      Serial.println(F("[ETH] Reconnected via DHCP."));
      #endif
    } else {
      #if DEBUG_SERIAL
      Serial.println(F("[ETH] Still offline after retry."));
      #endif
    }
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= LOG_PERIOD) {
    previousMillis = currentMillis;
    cpm = counts * multiplier; counts = 0;
    if (cpm > CPM_MAX_VALID) cpm = 0;
    usvh = cpm * CONV_FACTOR;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -40 && t < 80 && h >= 0 && h <= 100) {
      interiorTempC = 0.8f * interiorTempC + 0.2f * t;
      interiorHum   = 0.8f * interiorHum   + 0.2f * h;
    }

    if (cpm == 0) { if (zeroCount < 255) zeroCount++; } else zeroCount = 0;
    if (lastSuccessMillis && (currentMillis - lastSuccessMillis) > RADMON_FAIL_TIMEOUT_MS)
      lastFailReason = FAIL_NO_OK;

    if (netReady) {
      // network upload logic (omitted for brevity — same as v1.5.0)
    }

    updateHealthModel();
    updateStatusLED();
    wdt_reset();
  }

  ledTick();
}
