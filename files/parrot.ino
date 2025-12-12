/*
 * -------------------------------------------------------------
 *  Parrot Prime Firmware v1.5.1 (SafeBoot)
 *  Model: Parrot Prime (Leonardo + DFRobot PoE Shield)
 *  Author: Myndworx Asylum - Steven Sheeley
 *  Copyright 2025-2026 - MwA & Steven Sheeley
 *  License: MIT
 * -------------------------------------------------------------
 * Parrot is a simple background radiation monitoring system with
 * reporting capabilities. It's designed to monitor, of course,
 * background radiation, and environmental conditions internally
 * that can adversely affect its operation.
 * It sends a report to radmon.org, if there's an account there
 * for the reporting device, and it sends a ping to Parrot HQ
 * (The Nest).
 * -------------------------------------------------------------
 * This code is based on the original work of rjelbert at
 * https://github.com/rjelbert/geiger_counter
 * -------------------------------------------------------------
 * Version 1.5.1 “SafeBoot (Full, Sanitized)”
 *  - Safe watchdog + DHCP logic
 *  - 3× DHCP retry + 10 min periodic recovery
 *  - Non-blocking LED tick
 *  - DHT smoothing filter
 *  - Full Radmon + HQ telemetry restored
 * -------------------------------------------------------------
 */

#include <SPI.h>
#include <Ethernet.h>
#include <DHT.h>
#include <string.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <avr/io.h>

// ---------------- Debug Mode ----------------
#define DEBUG_SERIAL 1    // 1 = enable serial debugging

// ---------------- Firmware Info ----------------
#define FW_NAME     "Parrot"
#define FW_VERSION  "v1.5.1"
#define SERIAL_NUM  "ENTER_DEVICE_SERIAL_NUM"
#define FW_BUILD    FW_NAME " " FW_VERSION " (" __DATE__ " " __TIME__ ")"

// ---------------- Device Identity ----------------
#define STATUS_PING_ENABLE  1
#define STATUS_HOST         "www.myndworx.com"
#define STATUS_PORT         80
#define STATUS_PATH         "/parrot/api/status.php"

// ---------------- Credentials (Sanitized) ----------------
#define DEVICE_ID   "REPLACE_WITH_DEVICE_ID"
#define API_SECRET  "REPLACE_WITH_YOUR_SECRET"
#define RADMON_USER "REPLACE_WITH_YOUR_USERNAME"
#define RADMON_PASS "REPLACE_WITH_YOUR_PASSWORD"

// ---------------- Radiation Safety ----------------
#define CPM_NORMAL_MIN        20UL
#define CPM_NORMAL_MAX        40UL
#define CPM_WARN_THRESHOLD    100UL
#define CPM_DANGER_THRESHOLD  275UL
#define CPM_MAX_VALID         50000UL

enum RadState : uint8_t {
  RAD_LOW = 0, RAD_NORMAL, RAD_WARN, RAD_DANGER
};

// ---------------- DHT11 Sensor ----------------
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

#define TEMP_WARN_THRESHOLD_C 50.0f
#define TEMP_HIGH_THRESHOLD_C 60.0f

// ---------------- Ethernet (Leonardo + DFRobot PoE) ----------------
const byte mac[] = { 0x2E,0x3D,0x4E,0x5F,0x6E,0x7D };
const char server[] = "radmon.org";
EthernetClient client;
EthernetClient statusClient;

// ---------------- Timing ----------------
#define LOG_PERIOD 60000UL
#define MAX_PERIOD 60000UL
#define CONV_FACTOR 0.00812037f
#define RADMON_FAIL_TIMEOUT_MS (15UL*60UL*1000UL)
#define SS  10
#define RST 3

// ---------------- RGB LED ----------------
#define LED_R 5
#define LED_G 6
#define LED_B 7

void setStatusLED(byte r, byte g, byte b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

byte ledR=LOW, ledG=LOW, ledB=LOW;
bool ledFlash=false, ledOn=true;
unsigned long ledLastToggle=0;
const unsigned long LED_FLASH_PERIOD=500UL;

void applyLed() {
  if (ledOn) setStatusLED(ledR,ledG,ledB);
  else setStatusLED(LOW,LOW,LOW);
}
void setLedState(byte r,byte g,byte b,bool flash) {
  ledR=r; ledG=g; ledB=b;
  ledFlash=flash; ledOn=true;
  ledLastToggle=millis();
  applyLed();
}
void ledTick() {
  if (!ledFlash) return;
  unsigned long now=millis();
  if (now-ledLastToggle>=LED_FLASH_PERIOD) {
    ledLastToggle=now; ledOn=!ledOn; applyLed();
  }
}

// ---------------- Fail Reasons ----------------
enum FailReason:uint8_t{FAIL_NONE=0,FAIL_CONNECT,FAIL_NO_OK,FAIL_DHCP};

// ---------------- Globals ----------------
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
bool netReady=true;
uint8_t resetCause __attribute__((section(".noinit")));
#define ZERO_CPM_FAULT_COUNT 3

// ---------------- Capture Reset Cause ----------------
void getResetCause(void) __attribute__((naked)) __attribute__((section(".init3")));
void getResetCause(void){
  resetCause=MCUSR;
  MCUSR=0;
  wdt_disable();
}

// ---------------- ISR ----------------
void tube_impulse(){counts++;}

// ---------------- Radiation State Helper ----------------
RadState getRadiationState(unsigned long cpmValue){
  if(cpmValue>=CPM_DANGER_THRESHOLD)return RAD_DANGER;
  else if(cpmValue>=CPM_WARN_THRESHOLD)return RAD_WARN;
  else if(cpmValue>=CPM_NORMAL_MIN&&cpmValue<=CPM_NORMAL_MAX)return RAD_NORMAL;
  else return RAD_LOW;
}

// ---------------- Device Health Model ----------------
struct DeviceHealth{
  uint8_t level;
  RadState radState;
  bool sensorFault;
  bool tempWarn;
  bool tempHigh;
  bool netOK;
};
DeviceHealth health;

void updateHealthModel(){
  health.radState=getRadiationState(cpm);
  health.sensorFault=(zeroCount>=ZERO_CPM_FAULT_COUNT);
  health.tempWarn=(interiorTempC>=TEMP_WARN_THRESHOLD_C&&interiorTempC<TEMP_HIGH_THRESHOLD_C);
  health.tempHigh=(interiorTempC>=TEMP_HIGH_THRESHOLD_C);

  bool netOKLocal;
  if(lastFailReason==FAIL_DHCP)netOKLocal=false;
  else if(lastSuccessMillis==0)netOKLocal=false;
  else netOKLocal=(millis()-lastSuccessMillis<=RADMON_FAIL_TIMEOUT_MS);
  health.netOK=netOKLocal;

  uint8_t lvl=0;
  if(!health.netOK)lvl=max(lvl,(uint8_t)1);
  if(health.sensorFault||health.tempWarn||health.radState==RAD_WARN)lvl=max(lvl,(uint8_t)2);
  if(health.tempHigh||health.radState==RAD_DANGER)lvl=max(lvl,(uint8_t)3);
  health.level=lvl;
}

// ---------------- LED Logic ----------------
void updateStatusLED(){
  switch(health.level){
    case 3:setLedState(HIGH,LOW,LOW,true);break;    // Flashing red
    case 2:setLedState(HIGH,LOW,LOW,false);break;   // Solid red
    case 1:setLedState(HIGH,HIGH,LOW,false);break;  // Yellow
    default:setLedState(LOW,HIGH,LOW,false);break;  // Green
  }
}
// ---------------- DHCP SafeBoot Logic ----------------
bool attemptDHCP() {
  int retries = 3;
  while (retries-- > 0) {
    if (Ethernet.begin(mac) != 0) return true;
#if DEBUG_SERIAL
    Serial.println(F("[ETH] DHCP failed, retrying in 5 s..."));
#endif
    for (int i = 0; i < 10; i++) { // 10 × 0.5 s = 5 s
      wdt_reset();
      delay(500);
    }
  }
  return false;
}

// ---------------- HTTP Status Reader ----------------
int readHttpStatusCode(EthernetClient &c, unsigned long timeoutMs) {
  unsigned long start = millis();
  char line[64];
  byte idx = 0;
  bool gotStatusLine = false;
  int statusCode = 0;

  while (millis() - start < timeoutMs && c.connected()) {
    if (c.available()) {
      char ch = c.read();
      if (ch == '\r') continue;
      if (ch == '\n') {
        line[idx] = '\0';
        if (!gotStatusLine && strncmp(line, "HTTP/", 5) == 0) {
          char *p = strchr(line, ' ');
          if (p != NULL) {
            statusCode = atoi(p + 1);
            gotStatusLine = true;
            break;
          }
        }
        idx = 0;
      } else if (idx < (sizeof(line) - 1)) line[idx++] = ch;
    }
  }
  if (!gotStatusLine) return 0;
  return statusCode;
}

// ---------------- Radmon Upload ----------------
void uploadToRadmon() {
#if DEBUG_SERIAL
  Serial.println(F("[RAD] Connecting to radmon.org..."));
#endif
  if (client.connect(server, 80)) {
    char v[12];
    snprintf(v, sizeof(v), "%lu", cpm);
    char radGET[200];
    snprintf(radGET, sizeof(radGET),
      "GET /radmon.php?function=submit&user=%s&password=%s&value=%s&unit=CPM HTTP/1.1",
      RADMON_USER, RADMON_PASS, v);
    client.println(radGET);
    client.println(F("Host: radmon.org"));
    client.println(F("User-Agent: Parrot Prime SafeBoot"));
    client.println(F("Connection: close"));
    client.println();

    int statusCode = readHttpStatusCode(client, 5000UL);
    lastHttpStatusCode = statusCode;
    if (statusCode == 200 || statusCode == 204) {
      lastFailReason = FAIL_NONE;
      lastSuccessMillis = millis();
#if DEBUG_SERIAL
      Serial.println(F("[RAD] Upload OK"));
#endif
    } else {
      lastFailReason = FAIL_NO_OK;
#if DEBUG_SERIAL
      Serial.println(F("[RAD] Upload FAILED"));
#endif
    }
    client.stop();
  } else {
    lastFailReason = FAIL_CONNECT;
    lastHttpStatusCode = 0;
#if DEBUG_SERIAL
    Serial.println(F("[RAD] radmon.org connect failed."));
#endif
    client.stop();
  }
}

// ---------------- HQ Status Ping ----------------
void sendStatusPing() {
#if STATUS_PING_ENABLE
  if (Ethernet.linkStatus() != LinkON) return;
  if (!API_SECRET[0]) return;

  if (statusClient.connect(STATUS_HOST, STATUS_PORT)) {
    unsigned long up = (millis() - bootMillis) / 1000UL;

    const char *radStr;
    switch (health.radState) {
      case RAD_LOW: radStr = "LOW"; break;
      case RAD_NORMAL: radStr = "NORM"; break;
      case RAD_WARN: radStr = "WARN"; break;
      case RAD_DANGER: radStr = "DANGER"; break;
      default: radStr = "LOW"; break;
    }
    const char *failStr;
    switch (lastFailReason) {
      case FAIL_NONE: failStr = "NONE"; break;
      case FAIL_CONNECT: failStr = "CONNECT"; break;
      case FAIL_NO_OK: failStr = "NO_OK"; break;
      case FAIL_DHCP: failStr = "DHCP"; break;
      default: failStr = "UNK"; break;
    }

    statusClient.print("GET ");
    statusClient.print(STATUS_PATH);
    statusClient.print("?id=");
    statusClient.print(DEVICE_ID);
    statusClient.print("&sn=");
    statusClient.print(SERIAL_NUM);
    statusClient.print("&secret=");
    statusClient.print(API_SECRET);
    statusClient.print("&fw=");
    statusClient.print(FW_VERSION);
    statusClient.print("&up=");
    statusClient.print(up);
    statusClient.print("&cpm=");
    statusClient.print(cpm);
    statusClient.print("&rad=");
    statusClient.print(radStr);
    statusClient.print("&health=");
    statusClient.print(health.level);
    statusClient.print("&http=");
    statusClient.print(lastHttpStatusCode);
    statusClient.print("&temp=");
    if (interiorTempC > -100.0f) statusClient.print(interiorTempC, 1);
    statusClient.print("&hum=");
    if (interiorHum >= 0.0f) statusClient.print((int)(interiorHum + 0.5f));
    statusClient.print("&z=");
    statusClient.print(zeroCount);
    statusClient.print("&fail=");
    statusClient.print(failStr);
    statusClient.print("&reset=");
    statusClient.print(resetCause, HEX);
    statusClient.println(" HTTP/1.1");
    statusClient.print("Host: ");
    statusClient.println(STATUS_HOST);
    statusClient.println("Connection: close");
    statusClient.println();

    int statusCode = readHttpStatusCode(statusClient, 5000UL);
    hqHttpStatusCode = statusCode;
    statusClient.stop();
#if DEBUG_SERIAL
    Serial.print(F("[HQ] Ping status code: "));
    Serial.println(statusCode);
#endif
  }
#endif
}

// ---------------- Setup ----------------
void setup() {
  counts = 0; cpm = 0; multiplier = MAX_PERIOD / LOG_PERIOD;
  previousMillis = millis(); bootMillis = millis();
  lastFailReason = FAIL_NONE; lastHttpStatusCode = 0;
  hqHttpStatusCode = 0; interiorTempC = -127.0f;
  interiorHum = -1.0f; zeroCount = 0; lastSuccessMillis = 0;
  netReady = true;

  Serial.begin(9600);
#if DEBUG_SERIAL
  unsigned long serialStart = millis();
  while (!Serial && (millis() - serialStart < 3000UL)) {;}
#endif
  Serial.println(); Serial.println(FW_BUILD);

  attachInterrupt(digitalPinToInterrupt(2), tube_impulse, FALLING);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLedState(LOW, HIGH, LOW, false);
  dht.begin();

  // PoE shield reset
  pinMode(SS, OUTPUT); pinMode(RST, OUTPUT);
  digitalWrite(SS, HIGH); digitalWrite(RST, HIGH);
  delay(200); digitalWrite(RST, LOW);
  delay(200); digitalWrite(RST, HIGH);
  delay(200); digitalWrite(SS, LOW);
  Ethernet.init(10);
  delay(1000);

  // SafeBoot DHCP init
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
    Serial.println(F("[ETH] DHCP failed after retries. Offline mode."));
#endif
  }

  wdt_enable(WDTO_8S);
  updateHealthModel();
  updateStatusLED();
#if DEBUG_SERIAL
  Serial.println(F("[BOOT] System initialized. SafeBoot active."));
#endif
}
// ---------------- Main Loop ----------------
void loop() {
  wdt_reset();
  Ethernet.maintain();

  // Periodic DHCP retry if offline (every 10 min)
  static unsigned long lastDhcpRetry = 0;
  if (!netReady && (millis() - lastDhcpRetry >= 600000UL)) {
    lastDhcpRetry = millis();
#if DEBUG_SERIAL
    Serial.println(F("[ETH] Retrying DHCP..."));
#endif
    if (attemptDHCP()) {
      netReady = true;
      lastFailReason = FAIL_NONE;
      lastSuccessMillis = millis();
#if DEBUG_SERIAL
      Serial.print(F("[ETH] Reconnected. IP: "));
      Serial.println(Ethernet.localIP());
#endif
    } else {
#if DEBUG_SERIAL
      Serial.println(F("[ETH] DHCP retry failed. Still offline."));
#endif
    }
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= LOG_PERIOD) {
    previousMillis = currentMillis;

    // ---- CPM calculation ----
    cpm = counts * multiplier;
    counts = 0;
    if (cpm > CPM_MAX_VALID) cpm = 0;
    usvh = cpm * CONV_FACTOR;

    // ---- DHT sampling ----
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -40 && t < 80 && h >= 0 && h <= 100) {
      // simple exponential smoothing
      if (interiorTempC < -100) interiorTempC = t;
      else interiorTempC = 0.8f * interiorTempC + 0.2f * t;
      if (interiorHum < 0) interiorHum = h;
      else interiorHum = 0.8f * interiorHum + 0.2f * h;
    }

    // ---- Zero count tracking ----
    if (cpm == 0) {
      if (zeroCount < 255) zeroCount++;
    } else zeroCount = 0;

    // ---- Connection stale detection ----
    if (lastSuccessMillis &&
        (currentMillis - lastSuccessMillis) > RADMON_FAIL_TIMEOUT_MS)
      lastFailReason = FAIL_NO_OK;

#if DEBUG_SERIAL
    Serial.println(F("------------------------------------------------"));
    Serial.print(F("[DATA] CPM: ")); Serial.println(cpm);
    Serial.print(F("[DATA] uSv/h: ")); Serial.println(usvh, 3);
    Serial.print(F("[DATA] TempC: ")); Serial.println(interiorTempC, 1);
    Serial.print(F("[DATA] Hum: ")); Serial.println(interiorHum, 1);
    Serial.print(F("[DATA] ZeroCount: ")); Serial.println(zeroCount);
#endif

    // ---- Network operations ----
    if (netReady) {
      uploadToRadmon();
      sendStatusPing();
    }

    // ---- Health & LED update ----
    updateHealthModel();
    updateStatusLED();
    wdt_reset();
  }

  // Non-blocking LED flashing
  ledTick();
}
