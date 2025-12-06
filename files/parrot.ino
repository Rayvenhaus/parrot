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

// ----------------- Firmware Version -----------------

#define FW_NAME     "Parrot"
#define FW_VERSION  "v1.4.3"
#define FW_BUILD    FW_NAME " " FW_VERSION " (" __DATE__ " " __TIME__ ")"

// ----------------- Device Identity / Telemetry -----------------

#define DEVICE_ID           "PARROT-001"
#define API_SECRET          "REPLACE_WITH_YOUR_SECRET"

#define STATUS_PING_ENABLE  1
#define STATUS_HOST         "www.myndworx.com"
#define STATUS_PORT         80
#define STATUS_PATH         "/parrot/api/status.php"

// ----------------- Radiation Safety -----------------

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

#define HIGH_TEMP_THRESHOLD_C   50.0f

// ----------------- Ethernet + Radmon -----------------

const byte mac[]   = { 0x2E, 0x3D, 0x4E, 0x5F, 0x6E, 0x7D };
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

// ----------------- LED Logic -----------------

void updateStatusLED() {
    bool sensorError = (zeroCount >= ZERO_CPM_FAULT_COUNT);
    bool tempValid   = (interiorTempC > -100.0f);
    bool highTemp    = (tempValid && interiorTempC >= HIGH_TEMP_THRESHOLD_C);
    bool netOK       = (lastFailReason == FAIL_NONE);

    if (sensorError) {
        LED_ERROR;
    } else if (highTemp) {
        LED_WARN;
    } else if (netOK) {
        LED_OK;
    } else {
        LED_WARN;
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
            } else if (idx < sizeof(line) - 1) {
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
        Serial.println(F("[HQ] Status ping start"));

        if (Ethernet.linkStatus() != LinkON) {
            Serial.println(F("[HQ] Link DOWN, skipping ping"));
            return;
        }
        if (!API_SECRET[0]) {
            Serial.println(F("[HQ] API_SECRET not set, skipping ping"));
            return;
        }

        if (statusClient.connect(STATUS_HOST, STATUS_PORT)) {
            Serial.print(F("[HQ] Connected to "));
            Serial.print(STATUS_HOST);
            Serial.print(F(":"));
            Serial.println(STATUS_PORT);

            unsigned long up = (millis() - bootMillis) / 1000UL;
            RadState rs = getRadiationState(cpm);

            const char *radStr;
            switch (rs) {
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

            Serial.print(F("[HQ] Preparing query: "));
            Serial.print(F("GET "));
            Serial.print(STATUS_PATH);
            Serial.print(F("?id="));
            Serial.print(DEVICE_ID);
            Serial.print(F("&secret=****"));
            Serial.print(F("&fw="));
            Serial.print(FW_VERSION);
            Serial.print(F("&up="));
            Serial.print(up);
            Serial.print(F("&cpm="));
            Serial.print(cpm);
            Serial.print(F("&rad="));
            Serial.print(radStr);
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

            statusClient.print("GET ");
            statusClient.print(STATUS_PATH);
            statusClient.print("?id=");
            statusClient.print(DEVICE_ID);
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

            unsigned long start = millis();
            while (millis() - start < 2000UL && statusClient.connected()) {
                while (statusClient.available()) {
                    (void)statusClient.read();
                }
            }

            statusClient.stop();
            Serial.println(F("[HQ] Status ping complete, connection closed"));
        } else {
            Serial.print(F("[HQ] Connect failed to "));
            Serial.print(STATUS_HOST);
            Serial.print(F(":"));
            Serial.println(STATUS_PORT);
        }
    #endif
}

// ----------------- Radmon upload -----------------

void uploadToRadmon() {
    Serial.println(F("Connecting to radmon..."));
    if (client.connect(server, 80)) {
        char v[12];
        snprintf(v, sizeof(v), "%lu", cpm);

        client.print(F("GET /radmon.php?function=submit&user=YOUR_USERNAME&password=YOUR_PASSWORD&value="));
        client.print(v);
        client.println(F("&unit=CPM HTTP/1.1"));
        client.println(F("Host: radmon.org"));
        client.println(F("User-Agent: Parrot Geiger Counter Board"));
        client.println(F("Connection: close"));
        client.println();

        int statusCode = readHttpStatusCode(client, 5000UL);
        lastHttpStatusCode = statusCode;

        if (statusCode == 200 || statusCode == 204) {
            Serial.println(F("radmon.org update accepted!"));
            lastFailReason = FAIL_NONE;
            lastSuccessMillis = millis();
        } else {
            Serial.println(F("radmon.org update failed."));
            lastFailReason = FAIL_NO_OK;
        }
        client.stop();
    } else {
        lastFailReason = FAIL_CONNECT;
        lastHttpStatusCode = 0;
        Serial.println(F("radmon.org update did not connect."));
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
    interiorTempC = -127.0f;
    interiorHum   = -1.0f;
    zeroCount = 0;
    lastSuccessMillis = 0;
    netReady = true;

    Serial.begin(9600);
    delay(1000);
    Serial.println();
    Serial.print(F("Booting "));
    Serial.println(FW_BUILD);

    pinMode(2, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(2), tube_impulse, FALLING);

    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    LED_OK;

    dht.begin();

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
    }

    wdt_enable(WDTO_8S);
    updateStatusLED();
    Serial.println(F("Boot sequence completed, system initialized."));
}

// ----------------- Main loop -----------------

void loop() {
    wdt_reset();
    Ethernet.maintain();

    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= LOG_PERIOD) {
        previousMillis = currentMillis;

        noInterrupts();
        unsigned long localCounts = counts;
        counts = 0;
        interrupts();

        cpm = localCounts * multiplier;
        if (cpm > CPM_MAX_VALID) cpm = 0;
        usvh = cpm * CONV_FACTOR;

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h) &&
            t > -40.0f && t < 80.0f &&
            h >= 0.0f && h <= 100.0f) {
            interiorTempC = t;
            interiorHum   = h;
        }

        Serial.print(F("Temp: "));
        Serial.println(interiorTempC);
        Serial.print(F("Hum: "));
        Serial.println(interiorHum);

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

        updateStatusLED();
        wdt_reset();
    }
}
