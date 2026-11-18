#include <Arduino.h>
#include <NimBLEDevice.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <atomic>
#include <esp_sntp.h>
#include <hal/gpio_ll.h>
#include <time.h>

#include "bench_storage.h"

namespace {
constexpr uint8_t servoPin = 17;  // M-Bus pin 16, normally UART2 TX.
constexpr uint8_t servoChannel = 4;
constexpr uint8_t servoResolution = 16;
constexpr uint32_t servoFrequency = 50;
constexpr uint32_t servoPeriodUs = 1000000 / servoFrequency;
constexpr uint32_t servoHoldMs = 1500;
constexpr uint16_t servoPulses[] = {1500, 1300, 1700};

int wifiCount = -1;
int bleCount = -1;
int wifiChannelCount = 0;
bool bleReady = false;
bool servoActive = false;
size_t nextPulse = 0;
uint32_t servoStarted = 0;
std::atomic<bool> clockSynced{false};
time_t displayedSecond = 0;
bool sdMounted = false;

uint8_t mijiaAddress[mijiaAddressSize] = {};
bool mijiaEnabled = false;
bool mijiaAddressPending = false;
float indoorTemperature = NAN;
unsigned indoorHumidity = 0;

void refreshClock(bool redraw = false) {
    const time_t now = clockSynced.load() ? time(nullptr) : 0;
    if (!redraw && now == displayedSecond) {
        return;
    }
    char timestamp[24] = "Clock: waiting for sync";
    if (now != 0) {
        tm local{};
        localtime_r(&now, &local);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    }
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(12, 166);
    M5.Display.fillRect(12, 166, 296, 16, TFT_BLACK);
    M5.Display.print(timestamp);
    displayedSecond = now;
}

void logEvent(const char* event) {
    if (sdMounted && !appendLog(event, clockSynced.load())) {
        Serial.println("[SD] log write failed");
    }
}

void drawBleLine() {
    auto& display = M5.Display;
    display.fillRect(12, 108, 296, 16, TFT_BLACK);
    display.setCursor(12, 108);
    if (mijiaEnabled && !isnan(indoorTemperature)) {
        display.printf("Room: %.1f C  %u %%", indoorTemperature, indoorHumidity);
    } else if (mijiaEnabled) {
        display.print("Mijia: waiting for data");
    } else if (bleCount >= 0) {
        display.printf("BLE:  %d devices", bleCount);
    } else {
        display.print("BLE:  --");
    }
}

void drawScreen(const char* status) {
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(12, 12);
    display.println("NUANSHI / CORE TEST");
    display.fillRect(12, 40, 72, 24, TFT_RED);
    display.fillRect(86, 40, 72, 24, TFT_GREEN);
    display.fillRect(160, 40, 72, 24, TFT_BLUE);
    display.fillRect(234, 40, 72, 24, TFT_WHITE);
    display.setCursor(12, 82);
    if (WiFi.status() == WL_CONNECTED) {
        display.print("WiFi: ");
        display.println(WiFi.localIP());
    } else if (wifiCount >= 0) {
        display.printf("WiFi: %d APs / %d ch\n", wifiCount, wifiChannelCount);
    } else {
        display.println("WiFi: --");
    }
    drawBleLine();
    display.setCursor(12, 143);
    display.println(status);
    display.setCursor(12, 193);
    display.println("A WiFi  B BLE  C PWM");
    display.setCursor(12, 218);
    display.println(sdMounted ? "microSD: mounted" : "microSD: unavailable");
    refreshClock(true);
}

void scanWifi() {
    drawScreen("Scanning WiFi...");
    wifiChannelCount = 0;
    const bool wifiWasOn = WiFi.getMode() != WIFI_MODE_NULL;
    if (WiFi.mode(WIFI_STA)) {
        wifiCount = WiFi.scanNetworks(false, true);
        uint32_t channels = 0;
        for (int i = 0; i < wifiCount; ++i) {
            const int channel = WiFi.channel(i);
            if (channel >= 1 && channel <= 14) {
                channels |= 1UL << channel;
            }
        }
        wifiChannelCount = __builtin_popcount(channels);
        WiFi.scanDelete();
        if (!wifiWasOn) {
            WiFi.mode(WIFI_OFF);
        }
    } else {
        wifiCount = -1;
    }
    Serial.printf("[WIFI] count=%d channels=%d\n", wifiCount, wifiChannelCount);
    logEvent(wifiCount >= 0 ? "WiFi scan completed" : "WiFi scan failed");
    drawScreen(wifiCount > 0 ? "WiFi RX OK" : wifiCount == 0 ? "No WiFi APs seen" : "WiFi scan failed");
}

void connectAndSync(const WifiCredentials& credentials, bool saveOnConnect) {
    drawScreen("Connecting WiFi...");
    WiFi.disconnect();
    WiFi.begin(credentials.ssid, credentials.password);
    const uint32_t connectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 20000) {
        refreshClock();
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[NET] connect failed status=%d\n", WiFi.status());
        logEvent("WiFi connection failed");
        drawScreen("WiFi connect failed");
        return;
    }

    Serial.printf("[NET] connected ip=%s rssi=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    logEvent("WiFi connected");
    if (saveOnConnect) {
        const bool saved = saveCredentials(credentials);
        Serial.println(saved ? "[SD] WiFi credentials saved" : "[SD] WiFi credentials not saved");
        if (saved) {
            logEvent("WiFi credentials saved");
        }
    }
    drawScreen("Syncing time...");
    static std::atomic<bool> freshSync{false};
    freshSync.store(false);
    sntp_set_time_sync_notification_cb([](timeval*) {
        clockSynced.store(true);
        freshSync.store(true);
    });
    configTzTime("CST-8", "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
    const uint32_t syncStart = millis();
    while (!freshSync.load() && millis() - syncStart < 20000) {
        refreshClock();
        delay(100);
    }

    tm localTime{};
    if (!freshSync.load() || !getLocalTime(&localTime, 250)) {
        Serial.println("[SNTP] sync timed out");
        logEvent("SNTP sync timed out");
        drawScreen("SNTP sync timed out");
        return;
    }
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);
    Serial.printf("[SNTP] synced local=%s UTC+08:00\n", timestamp);
    logEvent("SNTP sync OK");
    drawScreen("SNTP sync OK");
}

bool initializeBle() {
    if (!bleReady) {
        NimBLEDevice::init("NuanShi");
        bleReady = NimBLEDevice::isInitialized();
    }
    return bleReady;
}

bool enableMijia() {
    if (mijiaEnabled || !initializeBle()) {
        return false;
    }
    mijiaEnabled = startMijiaReader(mijiaAddress);
    return mijiaEnabled;
}

void pollMijia() {
    MijiaReading reading{};
    if (!nextMijiaReading(reading)) {
        return;
    }
    if (mijiaAddressPending) {
        mijiaAddressPending = false;
        Serial.println(saveMijiaAddress(mijiaAddress) ? "[MIJIA] address saved to SD" : "[MIJIA] address active in RAM only");
    }
    indoorTemperature = reading.temperature;
    indoorHumidity = reading.humidity;
    char line[72];
    snprintf(line, sizeof(line), "Mijia temperature=%.2f C humidity=%u %% battery=%u mV",
             reading.temperature, reading.humidity, reading.batteryMillivolts);
    Serial.printf("[MIJIA] %s\n", line);
    logEvent(line);
    drawBleLine();
}

void scanBle() {
    if (mijiaEnabled) {
        drawScreen("Mijia BLE reader active");
        return;
    }
    drawScreen("Scanning BLE...");
    initializeBle();
    if (!bleReady) {
        Serial.println("[BLE] init failed");
        drawScreen("BLE init failed");
        return;
    }

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    NimBLEScanResults results = scan->getResults(5000, false);
    bleCount = results.getCount();
    scan->clearResults();
    Serial.printf("[BLE] count=%d\n", bleCount);
    logEvent("BLE scan completed");
    drawScreen(bleCount > 0 ? "BLE RX OK" : "No BLE devices seen");
}

void startServoPulse() {
    const uint16_t pulseUs = servoPulses[nextPulse];
    const uint32_t duty = (static_cast<uint32_t>(pulseUs) * (1UL << servoResolution) + servoPeriodUs / 2) / servoPeriodUs;
    const double frequency = ledcSetup(servoChannel, servoFrequency, servoResolution);
    if (frequency <= 0) {
        Serial.println("[PWM] configuration failed");
        drawScreen("PWM setup failed");
        return;
    }
    ledcAttachPin(servoPin, servoChannel);
    ledcWrite(servoChannel, duty);
    servoStarted = millis();
    servoActive = true;
    nextPulse = (nextPulse + 1) % (sizeof(servoPulses) / sizeof(servoPulses[0]));
    // Enable pad readback without changing the GPIO matrix output from LEDC.
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(servoPin));
    const unsigned long highUs = pulseIn(servoPin, HIGH, 60000);
    const unsigned long lowUs = pulseIn(servoPin, LOW, 60000);
    Serial.printf("[PWM] GPIO%u freq=%.2f Hz pulse=%u us duty=%lu\n", servoPin, frequency, pulseUs,
                  static_cast<unsigned long>(ledcRead(servoChannel)));
    Serial.printf("[PWM] pad high=%lu us low=%lu us\n", highUs, lowUs);
    char status[32];
    snprintf(status, sizeof(status), "PWM GPIO%u: %u us", servoPin, pulseUs);
    drawScreen(status);
}

void reportBoard() {
    Serial.printf("[BOARD] NuanShi bench flash=%lu LCD=%dx%d board=%d\n",
                  static_cast<unsigned long>(ESP.getFlashChipSize()), M5.Display.width(), M5.Display.height(),
                  static_cast<int>(M5.getBoard()));
}
}  // namespace

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    auto config = M5.config();
    config.internal_spk = false;
    M5.begin(config);
    if (M5.Display.width() < M5.Display.height()) {
        M5.Display.setRotation((M5.Display.getRotation() + 1) % 4);
    }
    pinMode(servoPin, OUTPUT);
    digitalWrite(servoPin, LOW);
    sdMounted = mountStorage();
    Serial.println(sdMounted ? "[SD] mounted" : "[SD] mount failed");
    logEvent("Boot");
    drawScreen("Ready");
    reportBoard();
    Serial.println("[HELP] ? board, A/w WiFi, B/b BLE, C/p PWM, n WiFi/SNTP, d SD, t clock, m Mijia");
    WifiCredentials credentials;
    if (loadCredentials(credentials)) {
        Serial.println("[SD] WiFi credentials loaded");
        connectAndSync(credentials, false);
    }
    if (loadMijiaAddress(mijiaAddress) && enableMijia()) {
        Serial.println("[MIJIA] address loaded from SD");
        drawBleLine();
    }
}

void loop() {
    M5.update();
    const int command = servoActive ? -1 : Serial.read();
    if (servoActive) {
        if (millis() - servoStarted >= servoHoldMs) {
            ledcWrite(servoChannel, 0);
            ledcDetachPin(servoPin);
            pinMode(servoPin, OUTPUT);
            digitalWrite(servoPin, LOW);
            servoActive = false;
            Serial.println("[PWM] output stopped");
            logEvent("PWM test completed");
            drawScreen("PWM output stopped");
        }
    } else if (command == 'n') {
        // Frame: 'n', SSID length, password length, then both byte strings.
        WifiCredentials credentials;
        if (readCredentials(Serial, credentials)) {
            connectAndSync(credentials, true);
        } else {
            Serial.println("[NET] credential frame invalid");
        }
    } else if (command == 'd') {
        const bool verified = appendLog("SD read/write test", clockSynced.load(), true);
        if (!verified) {
            Serial.println("[SD] log verification failed");
        }
        drawScreen(verified ? "SD log verified" : "SD log test failed");
    } else if (command == 't') {
        refreshClock();
        Serial.printf("[CLOCK] displayed_epoch=%lld\n", static_cast<long long>(displayedSecond));
    } else if (command == 'm') {
        uint8_t address[mijiaAddressSize];
        if (Serial.readBytes(address, sizeof(address)) != sizeof(address) || mijiaEnabled) {
            Serial.println("[MIJIA] address rejected");
        } else {
            memcpy(mijiaAddress, address, sizeof(address));
            if (enableMijia()) {
                mijiaAddressPending = true;
                Serial.println("[MIJIA] reading LYWSD03MMC");
                drawScreen("Mijia BLE reader active");
            } else {
                Serial.println("[MIJIA] could not start");
            }
        }
    } else if (M5.BtnA.wasPressed() || command == 'w') {
        scanWifi();
    } else if (M5.BtnB.wasPressed() || command == 'b') {
        scanBle();
    } else if (M5.BtnC.wasPressed() || command == 'p') {
        startServoPulse();
    } else if (command == '?') {
        reportBoard();
    }
    if (!servoActive) {
        pollMijia();
    }
    refreshClock();
    delay(10);
}
