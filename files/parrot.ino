/*
 * Original core code forked from: rjelbert/geiger_counter
 * Original Author: JiangJie Zhang, cajoetech@qq.com
 * License: MIT License
 */

#include <SPI.h>
#include <Ethernet.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <string.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <avr/io.h>

// ----------------- Firmware Version -----------------

#define FW_NAME     "Parrot"
#define FW_VERSION  "v1.3.1"
#define FW_BUILD    FW_NAME " " FW_VERSION " (" __DATE__ " " __TIME__ ")"

// ----------------- Device Identity / Telemetry -----------------

#define DEVICE_ID           "PARROT-001"

// Set to 1 and configure host/paths when you have a backend ready
#define STATUS_PING_ENABLE  0
#define STATUS_HOST         "your.server.tld"
#define STATUS_PORT         80
#define STATUS_PATH         "/status"

// ----------------- Radiation Safety Thresholds -----------------

#define CPM_NORMAL_MIN      20UL
#define CPM_NORMAL_MAX      40UL
#define CPM_WARN_THRESHOLD  100UL
#define CPM_DANGER_THRESHOLD 275UL

enum RadState : uint8_t {
    RAD_LOW = 0,
    RAD_NORMAL = 1,
    RAD_WARN = 2,
    RAD_DANGER = 3
};

// ----------------- DHT11 Interior Temp/Humidity -----------------

#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// High-temperature threshold (Celsius) for WARNING state
#define HIGH_TEMP_THRESHOLD_C 50.0f

// ----------------- Ethernet / radmon -----------------

byte mac[] = { 0x2E, 0x3D, 0x4E, 0x5F, 0x6E, 0x7D };
char server[] = "radmon.org";    // name address for server (using DNS)

EthernetClient client;

// Optional separate client for status ping (reuses stack)
EthernetClient statusClient;

// ----------------- Timing & conversion -----------------

#define LOG_PERIOD   60000UL      // Logging period in ms (recommended 15000-60000)
#define MAX_PERIOD   60000UL      // Maximum logging period without modifying this sketch
#define CONV_FACTOR  0.00812037f  // CPM -> uSv/h conversion factor

#define SS           10           // W5500 CS
#define RST          3            // W5500 RST

// ----------------- RGB System Status LED -----------------

#define LED_R        5
#define LED_G        6
#define LED_B        7

void setStatusLED(byte r, byte g, byte b) {
    digitalWrite(LED_R, r);
    digitalWrite(LED_G, g);
    digitalWrite(LED_B, b);
}

// Common-cathode RGB: HIGH = ON, LOW = OFF
#define LED_OK       setStatusLED(LOW, HIGH, LOW)    // Green
#define LED_WARN     setStatusLED(HIGH, HIGH, LOW)   // Yellow
#define LED_ERROR    setStatusLED(HIGH, LOW, LOW)    // Red

// ----------------- Button for display toggle / service mode -----------------

#define BTN_DISPLAY  8

// ----------------- Fail reason codes -----------------

#define FAIL_NONE     0
#define FAIL_CONNECT  1
#define FAIL_NO_OK    2
#define FAIL_DHCP     3

// ----------------- OLED debug display -----------------

Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ----------------- Globals -----------------

volatile unsigned long counts;      // GM tube pulse count (ISR)
unsigned long cpm;                  // CPM value
unsigned int multiplier;            // CPM scaling factor
unsigned long previousMillis;       // last logging time
unsigned long logPeriodMs;          // effective log period (normal vs service mode)
float usvh;                         // uSv/h (still calculated, not shown)

byte okflag;                        // 2 = HTTP 2xx success, 0 = anything else
byte zeroCount = 0;                 // consecutive zero-CPM periods
unsigned long lastSuccessMillis = 0; // millis() of last successful HTTP 2xx from radmon
unsigned long bootMillis = 0;        // millis() at boot for uptime
byte lastFailReason = FAIL_NONE;     // last failure reason
int  lastHttpStatusCode = 0;         // last HTTP status code (0 = no status / timeout)

bool displayEnabled = false;         // true when user has toggled display ON
bool serviceMode = false;            // service mode flag (button held at boot)
unsigned long lastButtonPress = 0;   // debounce timer for button

float interiorTempC = -127.0f;       // last valid interior temp (DHT11)
float interiorHum   = -1.0f;         // last valid interior humidity (%)

// Reset cause (from MCUSR)
uint8_t resetCause __attribute__((section(".noinit")));

// ----------------- Reset cause capture (runs before main) -----------------

void getResetCause(void) __attribute__((naked)) __attribute__((section(".init3")));
void getResetCause(void) {
    resetCause = MCUSR;
    MCUSR = 0;
    wdt_disable();   // ensure watchdog is off at start
}

// ----------------- Helpers: LED state -----------------

void updateStatusLED() {
    // Priority:
    // 1) Sensor error (no pulses 3 periods) -> RED
    // 2) High temp (>= HIGH_TEMP_THRESHOLD_C) -> YELLOW
    // 3) Network OK (HTTP success, no failReason) -> GREEN
    // 4) Anything else (HTTP trouble, DHCP, etc.) -> YELLOW

    bool sensorError = (zeroCount >= 3);
    bool tempValid   = (interiorTempC > -100.0f);
    bool highTemp    = (tempValid && interiorTempC >= HIGH_TEMP_THRESHOLD_C);
    bool netOK       = (okflag == 2 && lastFailReason == FAIL_NONE);

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

// ----------------- ISR -----------------

void tube_impulse() {       // captures events from Geiger kit
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

// ----------------- Display update -----------------

void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);

    // Line 1: Firmware name + version
    display.setCursor(0, 0);
    display.println(FW_NAME " " FW_VERSION);

    // Line 2: Status
    display.print("Status: ");
    if (zeroCount >= 3) {
        display.println("ERROR");
    } else if (okflag == 2 && lastFailReason == FAIL_NONE) {
        display.println("OK");
    } else {
        display.println("WARN");
    }

    // Line 3: Last upload result
    display.print("LastUp: ");
    if (okflag == 2) {
        display.println("OK");
    } else {
        display.println("WAIT");
    }

    // Line 4: Time since last successful HTTP 2xx
    display.print("LastOK: ");
    if (lastSuccessMillis == 0) {
        display.println("N/A");
    } else {
        unsigned long since = (millis() - lastSuccessMillis) / 1000UL; // seconds
        unsigned int mins = since / 60;
        unsigned int secs = since % 60;

        display.print(mins);
        display.print("m ");
        display.print(secs);
        display.println("s");
    }

    // Line 5: Uptime
    display.print("Up: ");
    unsigned long up = (millis() - bootMillis) / 1000UL; // seconds
    unsigned int upH = up / 3600;
    unsigned int upM = (up % 3600) / 60;
    unsigned int upS = up % 60;

    if (upH < 10) display.print('0');
    display.print(upH);
    display.print(':');
    if (upM < 10) display.print('0');
    display.print(upM);
    display.print(':');
    if (upS < 10) display.print('0');
    display.println(upS);

    // Line 6: Zero-CPM streak + interior temp + humidity + hot marker
    bool tempValid   = (interiorTempC > -100.0f);
    bool highTemp    = (tempValid && interiorTempC >= HIGH_TEMP_THRESHOLD_C);

    display.print("Z:");
    display.print(zeroCount);
    display.print(" T:");
    if (!tempValid) {
        display.print("--.-C");
    } else {
        display.print(interiorTempC, 1);
        display.print("C");
        if (highTemp) {
            display.print("!");
        }
    }
    display.print(" H:");
    if (interiorHum < 0.0f) {
        display.println("--%");
    } else {
        display.print((int)(interiorHum + 0.5f));
        display.println("%");
    }

    // Line 7: Radiation band + Fail summary
    display.print("Rad: ");
    RadState rs = getRadiationState(cpm);
    switch (rs) {
        case RAD_LOW:
            display.print("LOW ");
            break;
        case RAD_NORMAL:
            display.print("NORM");
            break;
        case RAD_WARN:
            display.print("WARN");
            break;
        case RAD_DANGER:
            display.print("DANGER");
            break;
    }

    display.print(" F:");
    switch (lastFailReason) {
        case FAIL_NONE:
            display.println("NONE");
            break;
        case FAIL_CONNECT:
            display.println("CONN");
            break;
        case FAIL_NO_OK:
            display.println("HTTP");
            break;
        case FAIL_DHCP:
            display.println("DHCP");
            break;
        default:
            display.println("UNK");
            break;
    }

    // Line 8: HTTP status and IP (may wrap)
    display.print("HTTP:");
    display.print(lastHttpStatusCode);
    display.print(' ');

    if (lastHttpStatusCode == 0) {
        display.println("NET/TO");
    } else if (lastHttpStatusCode == 200) {
        display.println("OK");
    } else if (lastHttpStatusCode == 204) {
        display.println("NOCNT");
    } else if (lastHttpStatusCode == 301 || lastHttpStatusCode == 302) {
        display.println("REDIR");
    } else if (lastHttpStatusCode == 400) {
        display.println("BADREQ");
    } else if (lastHttpStatusCode == 401) {
        display.println("AUTH");
    } else if (lastHttpStatusCode == 403) {
        display.println("FORBID");
    } else if (lastHttpStatusCode == 404) {
        display.println("NOTFND");
    } else if (lastHttpStatusCode == 429) {
        display.println("RATELM");
    } else if (lastHttpStatusCode >= 500 && lastHttpStatusCode < 600) {
        display.println("SRVERR");
    } else if (lastHttpStatusCode >= 400 && lastHttpStatusCode < 500) {
        display.println("4xx");
    } else if (lastHttpStatusCode >= 300 && lastHttpStatusCode < 400) {
        display.println("3xx");
    } else if (lastHttpStatusCode >= 200 && lastHttpStatusCode < 300) {
        display.println("2xx");
    } else {
        display.println("UNK");
    }

    display.print("IP: ");
    IPAddress ip = Ethernet.localIP();
    for (byte i = 0; i < 4; i++) {
        display.print(ip[i], DEC);
        if (i < 3) {
            display.print('.');
        }
    }

    display.display();
}

// ----------------- Setup -----------------

void setup() {
    // Basic init
    delay(5000);   // boot delay
    counts = 0;
    cpm = 0;
    multiplier = MAX_PERIOD / LOG_PERIOD;   // CPM multiplier (1.0 if LOG_PERIOD == MAX_PERIOD)
    previousMillis = millis();
    bootMillis = millis();
    lastFailReason = FAIL_NONE;
    okflag = 0;
    lastHttpStatusCode = 0;
    displayEnabled = false;
    serviceMode = false;
    interiorTempC = -127.0f;
    interiorHum   = -1.0f;
    logPeriodMs = LOG_PERIOD;

    Serial.begin(9600);
    delay(1000);
    Serial.println();
    Serial.println(FW_BUILD);
    Serial.print("Reset cause flags: ");
    Serial.println(resetCause, BIN);

    // Decode reset cause
    Serial.print("Last reset: ");
    if (resetCause == 0) {
        Serial.println("Power-on or unknown (MCUSR cleared).");
    } else {
        if (resetCause & _BV(PORF))  Serial.print("POR ");
        if (resetCause & _BV(EXTRF)) Serial.print("EXT ");
        if (resetCause & _BV(BORF))  Serial.print("BOR ");
        if (resetCause & _BV(WDRF))  Serial.print("WDT ");
        // JTRF exists on some MCUs; ignore if not meaningful
        Serial.println();
    }

    Serial.println("Parrot booting...");

    // Geiger interrupt
    attachInterrupt(digitalPinToInterrupt(2), tube_impulse, FALLING);

    // RGB status LED
    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    LED_OK;    // provisional, will be updated each cycle

    // Display toggle button / service mode button
    pinMode(BTN_DISPLAY, INPUT_PULLUP);  // button to GND

    // DHT11
    dht.begin();

    // Bring up Ethernet
    pinMode(SS, OUTPUT);
    pinMode(RST, OUTPUT);
    digitalWrite(SS, HIGH);
    digitalWrite(RST, HIGH);
    delay(200);
    digitalWrite(RST, LOW);   // reset module
    delay(200);
    digitalWrite(RST, HIGH);
    delay(200);
    digitalWrite(SS, LOW);

    Ethernet.init(10);  // Most Arduino W5500 shields use pin 10
    delay(1000);

    Serial.println("Initialize Ethernet...");
    if (Ethernet.begin(mac) == 0) {
        Serial.println("Failed to configure Ethernet using DHCP.");
        lastFailReason = FAIL_DHCP;
    }
    delay(1000);

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("Ethernet shield was not found.");
    }

    if (Ethernet.hardwareStatus() == EthernetW5500) {
        Serial.println("W5500 Ethernet controller detected.");
    }

    if (Ethernet.linkStatus() == LinkON) {
        Serial.println("Ethernet cable is connected.");
    }

    // Print local IP
    Serial.print("IP address: ");
    for (byte thisByte = 0; thisByte < 4; thisByte++) {
        Serial.print(Ethernet.localIP()[thisByte], DEC);
        if (thisByte < 3) {
            Serial.print(".");
        }
    }
    Serial.println();

    // OLED init — start powered down
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 allocation failed");
    } else {
        display.clearDisplay();
        display.display();
        // Turn display OFF by default; only enabled on button press
        display.ssd1306_command(SSD1306_DISPLAYOFF);
    }

    // Check for service mode (button held at boot)
    delay(50);
    if (digitalRead(BTN_DISPLAY) == LOW) {
        serviceMode = true;
        logPeriodMs = 10000UL;  // faster logging in service mode (10s)
        displayEnabled = true;
        if (display.width() > 0) {
            display.ssd1306_command(SSD1306_DISPLAYON);
        }
        Serial.println("SERVICE MODE: Enabled (short log period, no radmon uploads).");
    }

    // Start hardware watchdog (8s timeout)
    wdt_enable(WDTO_8S);

    // Initial LED state
    updateStatusLED();
}

// ----------------- Helper: read HTTP status code -----------------

int readHttpStatusCode(EthernetClient &c, unsigned long timeoutMs) {
    unsigned long start = millis();
    char line[64];
    byte idx = 0;
    bool gotStatusLine = false;
    int statusCode = 0;

    while (millis() - start < timeoutMs && c.connected()) {
        if (c.available()) {
            char ch = c.read();
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                line[idx] = '\0';

                // First line that starts with HTTP/ is the status line
                if (!gotStatusLine && strncmp(line, "HTTP/", 5) == 0) {
                    char *p = strchr(line, ' ');
                    if (p != NULL) {
                        statusCode = atoi(p + 1);
                        gotStatusLine = true;
                        break; // we can bail as soon as we have the status
                    }
                }

                idx = 0; // reset for next line
            } else {
                if (idx < (sizeof(line) - 1)) {
                    line[idx++] = ch;
                }
            }
        }
    }

    if (!gotStatusLine) {
        return 0;   // treat as failure / timeout / no response
    }
    return statusCode;
}

// ----------------- Optional: status ping to mothership -----------------

void sendStatusPing() {
#if STATUS_PING_ENABLE
    if (!Ethernet.linkStatus() == LinkON) {
        return;
    }

    if (statusClient.connect(STATUS_HOST, STATUS_PORT)) {
        unsigned long up = (millis() - bootMillis) / 1000UL;
        RadState rs = getRadiationState(cpm);

        statusClient.print("GET ");
        statusClient.print(STATUS_PATH);
        statusClient.print("?id=");
        statusClient.print(DEVICE_ID);
        statusClient.print("&up=");
        statusClient.print(up);
        statusClient.print("&cpm=");
        statusClient.print(cpm);
        statusClient.print("&http=");
        statusClient.print(lastHttpStatusCode);
        statusClient.print("&rad=");

        switch (rs) {
            case RAD_LOW:    statusClient.print("LOW"); break;
            case RAD_NORMAL: statusClient.print("NORM"); break;
            case RAD_WARN:   statusClient.print("WARN"); break;
            case RAD_DANGER: statusClient.print("DANGER"); break;
        }

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
    }
#endif
}

// ----------------- Main loop -----------------

void loop() {
    wdt_reset();  // kick watchdog early in loop

    unsigned long currentMillis = millis();

    // --- Check display toggle button with debounce (unless in service mode) ---
    if (!serviceMode) {
        if (digitalRead(BTN_DISPLAY) == LOW) {          // button pressed (to GND)
            if (millis() - lastButtonPress > 300) {     // basic debounce
                displayEnabled = !displayEnabled;

                if (displayEnabled) {
                    display.ssd1306_command(SSD1306_DISPLAYON);
                } else {
                    display.ssd1306_command(SSD1306_DISPLAYOFF);
                }

                lastButtonPress = millis();
            }
        }
    }

    // Periodic logging block
    if (currentMillis - previousMillis >= logPeriodMs) {
        previousMillis = currentMillis;

        // --- Atomic read/reset of counts ---
        noInterrupts();
        unsigned long localCounts = counts;
        counts = 0;
        interrupts();

        // --- Calculate CPM and uSv/h ---
        cpm = localCounts * multiplier;
        if (cpm > 50000UL) {
            cpm = 0;
        }
        usvh = cpm * CONV_FACTOR;

        // --- Read DHT11 interior temp + humidity once per logging period ---
        float t = dht.readTemperature();  // Celsius
        float h = dht.readHumidity();     // %RH
        if (!isnan(t) && !isnan(h)) {
            interiorTempC = t;
            interiorHum   = h;
        } else {
            Serial.println(F("DHT11 read failed"));
        }

        RadState rs = getRadiationState(cpm);

        Serial.print(F("CPM: "));
        Serial.println(cpm);
        Serial.print(F("uSv/h: "));
        Serial.println(usvh);
        Serial.print(F("IntTempC: "));
        if (interiorTempC < -100.0f) {
            Serial.println(F("N/A"));
        } else {
            Serial.println(interiorTempC, 1);
        }
        Serial.print(F("IntHum%: "));
        if (interiorHum < 0.0f) {
            Serial.println(F("N/A"));
        } else {
            Serial.println(interiorHum, 1);
        }

        Serial.print(F("RadState: "));
        switch (rs) {
            case RAD_LOW:    Serial.println(F("LOW")); break;
            case RAD_NORMAL: Serial.println(F("NORMAL")); break;
            case RAD_WARN:   Serial.println(F("WARN")); break;
            case RAD_DANGER: Serial.println(F("DANGER")); break;
        }
        Serial.println();

        // --- Sensor long-term failure detection ---
        if (cpm == 0) {
            if (zeroCount < 255) zeroCount++;
        } else {
            zeroCount = 0;
        }

        // --- Format CPM as a string (no String class) ---
        char v[12];
        snprintf(v, sizeof(v), "%lu", cpm);

        // --- Ethernet lease maintenance ---
        Ethernet.maintain();

        // --- Send to radmon (CPM only), unless in service mode ---
        if (!serviceMode) {
            Serial.println(F("Connecting to radmon..."));
            if (client.connect(server, 80)) {
                Serial.println(F("Connected!"));

                client.print(F("GET /radmon.php?function=submit&user=YOUR_USERNAME&password=YOUR_PASSWORD&value="));
                client.print(v);
                client.println(F("&unit=CPM HTTP/1.1"));
                client.println(F("Host: radmon.org"));
                client.println(F("User-Agent: DIY Arduino Geiger Counter"));
                client.println(F("Connection: close"));
                client.println();

                // --- Read HTTP status code with timeout ---
                int statusCode = readHttpStatusCode(client, 5000UL); // 5s timeout
                lastHttpStatusCode = statusCode;

                // Treat 200 and 204 as success, everything else as failure
                if (statusCode == 200 || statusCode == 204) {
                    okflag = 2;
                    lastFailReason = FAIL_NONE;
                    lastSuccessMillis = millis();
                } else {
                    okflag = 0;
                    lastFailReason = FAIL_NO_OK;
                }

                Serial.print(F("HTTP code: "));
                Serial.println(statusCode);
                client.stop();
                Serial.println();

                // Optional status ping to mothership
                sendStatusPing();

            } else {
                Serial.println(F("Not able to connect. Will try again next cycle!"));
                lastFailReason = FAIL_CONNECT;
                lastHttpStatusCode = 0;  // no HTTP status read
                client.stop();
            }
        } else {
            // In service mode, we skip radmon and just keep local telemetry
            Serial.println(F("SERVICE MODE: Skipping radmon upload this cycle."));
        }

        // --- Update LED and display for the new state ---
        updateStatusLED();

        if (displayEnabled) {
            updateDisplay();
        }

        // Kick watchdog at end of healthy cycle
        wdt_reset();
    }
}
