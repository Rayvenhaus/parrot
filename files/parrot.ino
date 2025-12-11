/*
 * Original core code forked from: rjelbert/geiger_counter
 * Original Author: JiangJie Zhang, cajoetech@qq.com
 * License: MIT License
 */

#include <SPI.h>
#include <Ethernet.h>
#include <DHT.h>
#include <string.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <avr/io.h>

// ---------------- Debug Mode ----------------

#define DEBUG_SERIAL  0    // set to 1 to enable serial debugging, 0 for quiet firmware

// ----------------- Firmware Version -----------------

#define FW_NAME     "Parrot"
#define FW_VERSION  "v1.5.0"
#define SERIAL_NUM  "ENTER_DEVICE_SERIAL_NUM"
#define FW_BUILD    FW_NAME " " FW_VERSION " (" __DATE__ " " __TIME__ ")"

// ----------------- Device Identity / Telemetry -----------------

#define STATUS_PING_ENABLE  1
#define STATUS_HOST         "www.myndworx.com"
#define STATUS_PORT         80
#define STATUS_PATH         "/parrot/api/status.php"

// ------------------ Parrot HQ API Credentials ------------------
#define DEVICE_ID           "PARROT001"
#define API_SECRET          "REPLACE_WITH_YOUR_SECRET"

// ------------------- Radmon API Credentials --------------------

#define RADMON_USER         "REPLACE_WITH_YOUR_USERNAME"
#define RADMON_PASS         "REPLACE_WITH_YOUR_PASSWORD"

// ---------------------- Radiation Safety -----------------------

#define CPM_NORMAL_MIN        20UL
#define CPM_NORMAL_MAX        40UL
#define CPM_WARN_THRESHOLD    100UL
#define CPM_DANGER_THRESHOLD  275UL
#define CPM_MAX_VALID         50000UL

enum RadState : uint8_t {
    RAD_LOW = 0,
    RAD_NORMAL = 1,
    RAD_WARN = 2,
    RAD_DANGER = 3
};

// ----------------- DHT11 -----------------

#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

#define TEMP_WARN_THRESHOLD_C   50.0f
#define TEMP_HIGH_THRESHOLD_C   60.0f

// ----------------- Ethernet + Radmon -----------------

const byte mac[]    = { 0x2E, 0x3D, 0x4E, 0x5F, 0x6E, 0x7D };
const char server[] = "radmon.org";

EthernetClient client;
EthernetClient statusClient;

// ----------------- Timing -----------------

#define LOG_PERIOD              60000UL
#define MAX_PERIOD              60000UL
#define CONV_FACTOR             0.00812037f
#define RADMON_FAIL_TIMEOUT_MS  (15UL * 60UL * 1000UL)

#define SS   10
#define RST  3

// ----------------- RGB LED -----------------

#define LED_R  5
#define LED_G  6
#define LED_B  7

void setStatusLED(byte r, byte g, byte b) {
    digitalWrite(LED_R, r);
    digitalWrite(LED_G, g);
    digitalWrite(LED_B, b);
}

#define LED_OK       setStatusLED(LOW, HIGH, LOW)
#define LED_WARN     setStatusLED(HIGH, HIGH, LOW)
#define LED_ERROR    setStatusLED(HIGH, LOW, LOW)

byte ledR = LOW;
byte ledG = LOW;
byte ledB = LOW;
bool ledFlash = false;
bool ledOn = true;
unsigned long ledLastToggle = 0;
const unsigned long LED_FLASH_PERIOD = 500UL;

void applyLed() {
    if (ledOn) {
        setStatusLED(ledR, ledG, ledB);
    } else {
        setStatusLED(LOW, LOW, LOW);
    }
}

void setLedState(byte r, byte g, byte b, bool flash) {
    ledR = r;
    ledG = g;
    ledB = b;
    ledFlash = flash;
    ledOn = true;
    ledLastToggle = millis();
    applyLed();
}

void ledTick() {
    if (!ledFlash) {
        return;
    }
    unsigned long now = millis();
    if (now - ledLastToggle >= LED_FLASH_PERIOD) {
        ledLastToggle = now;
        ledOn = !ledOn;
        applyLed();
    }
}

// ----------------- Fail Reasons -----------------

enum FailReason : uint8_t {
    FAIL_NONE    = 0,
    FAIL_CONNECT = 1,
    FAIL_NO_OK   = 2,
    FAIL_DHCP    = 3
};

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
int  lastHttpStatusCode;
int  hqHttpStatusCode;

struct DeviceHealth {
    uint8_t level;
    RadState radState;
    bool sensorFault;
    bool tempWarn;
    bool tempHigh;
    bool netOK;
};

DeviceHealth health;

float interiorTempC;
float interiorHum;

bool netReady = true;

uint8_t resetCause __attribute__((section(".noinit")));

#define ZERO_CPM_FAULT_COUNT  3

// ----------------- Capture reset cause -----------------

void getResetCause(void) __attribute__((naked)) __attribute__((section(".init3")));
void getResetCause(void) {
    resetCause = MCUSR;
    MCUSR = 0;
    wdt_disable();
}

// ----------------- ISR -----------------

void tube_impulse() {
    counts++;
}

// ----------------- Radiation state helper -----------------

RadState getRadiationState(unsigned long cpmValue) {
    if (cpmValue >= CPM_DANGER_THRESHOLD) {
        return RAD_DANGER;
    } else if (cpmValue >= CPM_WARN_THRESHOLD) {
        return RAD_WARN;
    } else if (cpmValue >= CPM_NORMAL_MIN && cpmValue <= CPM_NORMAL_MAX) {
        return RAD_NORMAL;
    } else {
        return RAD_LOW;
    }
}

// --------------------------New Health Model -----------------

void updateHealthModel() {
    health.radState = getRadiationState(cpm);

    health.sensorFault = (zeroCount >= ZERO_CPM_FAULT_COUNT);

    health.tempWarn = (interiorTempC >= TEMP_WARN_THRESHOLD_C &&
                       interiorTempC < TEMP_HIGH_THRESHOLD_C);

    health.tempHigh = (interiorTempC >= TEMP_HIGH_THRESHOLD_C);

    bool netOKLocal;
    if (lastFailReason == FAIL_DHCP) {
        netOKLocal = false;
    } else if (lastSuccessMillis == 0) {
        netOKLocal = false;
    } else {
        unsigned long now = millis();
        if (now - lastSuccessMillis > RADMON_FAIL_TIMEOUT_MS) {
            netOKLocal = false;
        } else {
            netOKLocal = true;
        }
    }
    health.netOK = netOKLocal;

    uint8_t lvl = 0;

    if (!health.netOK) {
        if (lvl < 1) lvl = 1;
    }

    if (health.sensorFault || health.tempWarn || health.radState == RAD_WARN) {
        if (lvl < 2) lvl = 2;
    }

    if (health.tempHigh || health.radState == RAD_DANGER) {
        if (lvl < 3) lvl = 3;
    }

    health.level = lvl;
}

// ----------------- LED Logic -----------------

void updateStatusLED() {
    switch (health.level) {
        case 3:
            // Catastrophic → FLASHING RED
            setLedState(HIGH, LOW, LOW, true);
            break;

        case 2:
            // Serious → SOLID RED
            setLedState(HIGH, LOW, LOW, false);
            break;

        case 1:
            // Warning → SOLID YELLOW
            setLedState(HIGH, HIGH, LOW, false);
            break;

        default:
            // Normal → SOLID GREEN
            setLedState(LOW, HIGH, LOW, false);
            break;
    }
}

// ----------------- HTTP status parsing -----------------

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
            } else if (idx < (sizeof(line) - 1)) {
                line[idx++] = ch;
            }
        }
    }
    if (!gotStatusLine) return 0;
    return statusCode;
}

// ----------------- Status ping to HQ -----------------

void sendStatusPing() {
    #if STATUS_PING_ENABLE
        #if DEBUG_SERIAL
        Serial.println(F("[HQ] Status ping start"));
        #endif

        if (Ethernet.linkStatus() != LinkON) {
            #if DEBUG_SERIAL
            Serial.println(F("[HQ] Link DOWN, skipping ping"));
            #endif
            return;
        }
        if (!API_SECRET[0]) {
            #if DEBUG_SERIAL
            Serial.println(F("[HQ] API_SECRET not set, skipping ping"));
            #endif
            return;
        }

        if (statusClient.connect(STATUS_HOST, STATUS_PORT)) {
            #if DEBUG_SERIAL
            Serial.print(F("[HQ] Connected to "));
            Serial.print(STATUS_HOST);
            Serial.print(F(":"));
            Serial.println(STATUS_PORT);
            #endif

            unsigned long up = (millis() - bootMillis) / 1000UL;
            RadState rs = getRadiationState(cpm);

            const char *radStr;
            switch (health.radState) {
                case RAD_LOW:    radStr = "LOW";    break;
                case RAD_NORMAL: radStr = "NORM";   break;
                case RAD_WARN:   radStr = "WARN";   break;
                case RAD_DANGER: radStr = "DANGER"; break;
                default:         radStr = "LOW";    break;
            }

            const char *failStr;
            switch (lastFailReason) {
                case FAIL_NONE:    failStr = "NONE";    break;
                case FAIL_CONNECT: failStr = "CONNECT"; break;
                case FAIL_NO_OK:   failStr = "NO_OK";   break;
                case FAIL_DHCP:    failStr = "DHCP";    break;
                default:           failStr = "UNK";     break;
            }

            #if DEBUG_SERIAL
            Serial.print(F("[HQ] Preparing query: GET "));
            Serial.print(STATUS_PATH);
            Serial.print(F("?id="));
            Serial.print(DEVICE_ID);
            Serial.print(F("&secret=****"));
            Serial.print(F("&sn="));
            Serial.print(SERIAL_NUM);
            Serial.print(F("&fw="));
            Serial.print(FW_VERSION);
            Serial.print(F("&up="));
            Serial.print(up);
            Serial.print(F("&cpm="));
            Serial.print(cpm);
            Serial.print(F("&rad="));
            Serial.print(radStr);
            Serial.print(F("&health="));
            Serial.print(health.level);
            Serial.print(F("&http="));
            Serial.print(lastHttpStatusCode);
            Serial.print(F("&temp="));
            if (interiorTempC > -100.0f) {
                Serial.print(interiorTempC, 1);
            }
            Serial.print(F("&hum="));
            if (interiorHum >= 0.0f) {
                Serial.print((int)(interiorHum + 0.5f));
            }
            Serial.print(F("&z="));
            Serial.print(zeroCount);
            Serial.print(F("&fail="));
            Serial.print(failStr);
            Serial.print(F("&reset="));
            Serial.println(resetCause, HEX);
            #endif

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
            if (interiorTempC > -100.0f) {
                statusClient.print(interiorTempC, 1);
            }
            statusClient.print("&hum=");
            if (interiorHum >= 0.0f) {
                statusClient.print((int)(interiorHum + 0.5f));
            }
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

            #if DEBUG_SERIAL
            Serial.print(F("[HQ] HTTP status: "));
            Serial.println(statusCode);

            char body[192];
            size_t idx = 0;
            unsigned long start = millis();
            while (millis() - start < 1000UL && statusClient.connected()) {
                while (statusClient.available()) {
                    char ch = statusClient.read();
                    if (idx < sizeof(body) - 1) {
                        body[idx++] = ch;
                    }
                }
            }
            body[idx] = '\0';

            Serial.print(F("[HQ] Raw body: "));
            Serial.println(body);
            #else
            // In non-debug mode, just drain any remaining data quickly
            unsigned long start = millis();
            while (millis() - start < 1000UL && statusClient.connected()) {
                while (statusClient.available()) {
                    (void)statusClient.read();
                }
            }
            #endif

            statusClient.stop();
            #if DEBUG_SERIAL
            Serial.println(F("[HQ] Status ping complete, connection closed"));
            #endif
        } else {
            #if DEBUG_SERIAL
            Serial.print(F("[HQ] Connect failed to "));
            Serial.print(STATUS_HOST);
            Serial.print(F(":"));
            Serial.println(STATUS_PORT);
            #endif
        }
    #endif
}

// ----------------- Radmon upload -----------------

void uploadToRadmon() {
    #if DEBUG_SERIAL
    Serial.println(F("Connecting to radmon..."));
    #endif

    if (client.connect(server, 80)) {
        char v[12];
        snprintf(v, sizeof(v), "%lu", cpm);

        #if DEBUG_SERIAL
        Serial.print(F("[RAD] Value string (v): "));
        Serial.println(v);
        #endif

        char radGET[200];
        snprintf(
            radGET,
            sizeof(radGET),
            "GET /radmon.php?function=submit&user=%s&password=%s&value=%s&unit=CPM HTTP/1.1",
            RADMON_USER,
            RADMON_PASS,
            v
        );

        #if DEBUG_SERIAL
        Serial.print(F("[RAD] Full request: "));
        Serial.println(radGET);
        #endif

        client.println(radGET);
        client.println(F("Host: radmon.org"));
        client.println(F("User-Agent: Parrot Geiger Counter Board"));
        client.println(F("Connection: close"));
        client.println();

        int statusCode = readHttpStatusCode(client, 5000UL);
        lastHttpStatusCode = statusCode;

        if (statusCode == 200 || statusCode == 204) {
            #if DEBUG_SERIAL
            Serial.println(F("radmon.org update accepted!"));
            #endif
            lastFailReason = FAIL_NONE;
            lastSuccessMillis = millis();
        } else {
            #if DEBUG_SERIAL
            Serial.println(F("radmon.org update failed."));
            #endif
            lastFailReason = FAIL_NO_OK;
        }
        client.stop();
    } else {
        lastFailReason = FAIL_CONNECT;
        lastHttpStatusCode = 0;
        #if DEBUG_SERIAL
        Serial.println(F("radmon.org update did not connect."));
        #endif
        client.stop();
    }
}

// ----------------- Setup -----------------

void setup() {
    counts = 0;
    cpm = 0;
    multiplier = MAX_PERIOD / LOG_PERIOD;
    previousMillis = millis();
    bootMillis = millis();
    lastFailReason = FAIL_NONE;
    lastHttpStatusCode = 0;
    hqHttpStatusCode = 0;
    interiorTempC = -127.0f;
    interiorHum   = -1.0f;
    zeroCount = 0;
    lastSuccessMillis = 0;
    netReady = true;

    Serial.begin(9600);
    #if DEBUG_SERIAL
        // Leonardo: wait briefly for the USB serial port to come up
        unsigned long serialStart = millis();
        while (!Serial && (millis() - serialStart < 3000UL)) {
            ; // wait up to 3 seconds for host to open the port
        }
    #endif
    Serial.println();
    Serial.println(FW_BUILD);

    attachInterrupt(digitalPinToInterrupt(2), tube_impulse, FALLING);

    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    setLedState(LOW, HIGH, LOW, false); // Solid green

    dht.begin();

    // Ethernet / PoE HAT reset sequence (matches original working behaviour)
    pinMode(SS, OUTPUT);
    pinMode(RST, OUTPUT);
    digitalWrite(SS, HIGH);
    digitalWrite(RST, HIGH);
    delay(200);
    digitalWrite(RST, LOW);
    delay(200);
    digitalWrite(RST, HIGH);
    delay(200);
    digitalWrite(SS, LOW);

    Ethernet.init(10);
    delay(1000);

    if (Ethernet.begin(mac) == 0) {
        lastFailReason = FAIL_DHCP;
        netReady = false;
        #if DEBUG_SERIAL
        Serial.println(F("Failed to configure Ethernet using DHCP."));
        #endif
    } else {
        #if DEBUG_SERIAL
        Serial.print(F("IP address: "));
        for (byte thisByte = 0; thisByte < 4; thisByte++) {
            Serial.print(Ethernet.localIP()[thisByte], DEC);
            if (thisByte < 3) {
                Serial.print(F("."));
            }
        }
        Serial.println();
        #endif
    }

    wdt_enable(WDTO_8S);
    updateStatusLED();

    #if DEBUG_SERIAL
    Serial.println(F("Boot sequence completed, system initialized."));
    #endif
}

// ----------------- Main loop -----------------

void loop() {
    wdt_reset();
    Ethernet.maintain();

    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= LOG_PERIOD) {
        previousMillis = currentMillis;

        // CPM calculation (match original working behaviour)
        cpm = counts * multiplier;
        counts = 0;

        if (cpm > CPM_MAX_VALID) {
            cpm = 0;
        }

        usvh = cpm * CONV_FACTOR;

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h) &&
            t > -40.0f && t < 80.0f &&
            h >= 0.0f && h <= 100.0f) {
            interiorTempC = t;
            interiorHum   = h;
        }

        #if DEBUG_SERIAL
        Serial.print(F("[PULSE] CPM this period: "));
        Serial.println(cpm);

        Serial.print(F("Temp: "));
        Serial.println(interiorTempC);

        Serial.print(F("Hum: "));
        Serial.println(interiorHum);

        Serial.print(F("CPM: "));
        Serial.println(cpm);

        if (cpm == 0) {
            Serial.println(F("!!! WARNING: CPM is ZERO. This is abnormal unless the tube is disconnected or dead."));
            Serial.println(F("!!! Sending CPM=0 to radmon.org"));
        }
        #endif

        if (cpm == 0) {
            if (zeroCount < 255) zeroCount++;
        } else {
            zeroCount = 0;
        }

        if (lastSuccessMillis != 0 &&
            (currentMillis - lastSuccessMillis) > RADMON_FAIL_TIMEOUT_MS) {
            lastFailReason = FAIL_NO_OK;
        }

        if (netReady) {
            uploadToRadmon();
            sendStatusPing();
        }

        updateHealthModel();
        updateStatusLED();
        wdt_reset();
    }

    ledTick();
}
