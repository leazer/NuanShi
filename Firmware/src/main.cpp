#include <Arduino.h>
#include <NimBLEDevice.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <atomic>
#include <esp_sntp.h>
#include <hal/gpio_ll.h>
#include <time.h>

#include "bench_storage.h"
#include "changping_weather.h"

namespace {
constexpr uint8_t servoPin = 17;  // M-Bus pin 16, normally UART2 TX.
constexpr uint8_t servoChannel = 4;
constexpr uint8_t servoResolution = 16;
constexpr uint32_t servoFrequency = 50;
constexpr uint32_t servoPeriodUs = 1000000 / servoFrequency;
constexpr uint32_t servoHoldMs = 1500;
constexpr uint16_t servoPulses[] = {1500, 1300, 1700};
constexpr uint32_t indoorStaleMs = mijiaSampleIntervalMs + 60 * 1000;
constexpr uint32_t indoorLostMs = mijiaSampleIntervalMs + 3 * 60 * 1000;
constexpr unsigned trendSamples = 6;  // Roughly ten minutes at the normal sample interval.

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
uint32_t indoorUpdatedMs = 0;
unsigned sensorFailures = 0;
float temperatureHistory[trendSamples] = {};
unsigned readingsInHistory = 0;
unsigned nextHistoryIndex = 0;
int targetTenths = 220;
bool settingsOpen = false;
bool weatherStarted = false;
bool wifiWasConnected = false;
OutdoorSample outdoor{NAN, 0, -1};
WebServer logServer(80);
char screenStatus[32] = "Ready";

bool hasOutdoorTemperature() {
    const double age = difftime(time(nullptr), outdoor.observedAt);
    return clockSynced.load() && isfinite(outdoor.temperature) && age >= -900 && age <= 5400;
}

void printAt(const char* text, int x, int y, uint16_t color = TFT_WHITE) {
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.setCursor(x, y);
    M5.Display.print(text);
}

void printCentered(const char* text, int centerX, int y) {
    printAt(text, centerX - M5.Display.textWidth(text) / 2, y);
}

void printTemperature(const char* value, int x, int y, uint16_t color = TFT_WHITE) {
    auto& display = M5.Display;
    const int unitX = x + display.textWidth(value) + 5;
    const int bottom = y + display.fontHeight();
    printAt(value, x, y, color);
    display.setFont(&fonts::efontCN_16);
    printAt("°C", unitX, bottom - display.fontHeight(), color);
}

void drawHeader() {
    auto& display = M5.Display;
    display.setFont(&fonts::efontCN_16);
    display.fillRect(0, 0, 320, 31, TFT_BLACK);
    printAt("暖时", 10, 8);
    char date[20] = "--/--  --:--";
    if (displayedSecond > 0) {
        tm local{};
        localtime_r(&displayedSecond, &local);
        strftime(date, sizeof(date), "%m/%d  %H:%M", &local);
    }
    printCentered(date, 160, 8);
    const bool connected = WiFi.status() == WL_CONNECTED;
    printAt("WiFi", 245, 8, connected ? TFT_WHITE : TFT_DARKGREY);
    const int rssi = connected ? WiFi.RSSI() : -127;
    for (int bar = 0; bar < 4; ++bar) {
        const int height = 4 + bar * 4;
        display.fillRect(281 + bar * 7, 23 - height, 5, height,
                         rssi >= -85 + 10 * bar ? TFT_GREEN : TFT_DARKGREY);
    }
}

void logEvent(const char* event) {
    if (sdMounted && !appendLog(event, clockSynced.load())) {
        Serial.println("[SD] log write failed");
    }
}

const char* roomTrend() {
    if (readingsInHistory < trendSamples) {
        return "趋势采集中";
    }
    const unsigned mostRecent = (nextHistoryIndex + trendSamples - 1) % trendSamples;
    const float change = temperatureHistory[mostRecent] - temperatureHistory[nextHistoryIndex];
    return change > 0.2f ? "室温上升中" : change < -0.2f ? "室温下降中" : "室温平稳";
}

bool roomHasAged(uint32_t thresholdMs) {
    return !isnan(indoorTemperature) && millis() - indoorUpdatedMs >= thresholdMs;
}

bool roomLost() {
    return sensorFailures >= mijiaMaxAttempts || roomHasAged(indoorLostMs);
}

bool roomStale() {
    return sensorFailures > 0 || roomHasAged(indoorStaleMs);
}

const char* roomState() {
    if (roomLost()) {
        return "温度计失联";
    }
    if (roomStale()) {
        return "室温过期 · 重试中";
    }
    return isnan(indoorTemperature) ? "等待温度计" : roomTrend();
}

void drawMeasurements() {
    auto& display = M5.Display;
    display.fillRect(0, 34, 320, 132, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    printAt("目标温度", 14, 38, TFT_CYAN);
    printAt("室外温度", 14, 102, TFT_LIGHTGREY);
    const uint16_t roomColor = roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : TFT_WHITE;
    printAt("室内温度", 157, 42, roomColor);

    char value[20];
    display.setFont(&fonts::FreeSansBold18pt7b);
    snprintf(value, sizeof(value), "%.1f", targetTenths / 10.0f);
    printTemperature(value, 14, 61);
    if (hasOutdoorTemperature()) {
        snprintf(value, sizeof(value), "%.1f", outdoor.temperature);
    } else {
        strcpy(value, "--");
    }
    display.setFont(&fonts::FreeSansBold18pt7b);
    printTemperature(value, 14, 125);
    display.setFont(&fonts::FreeSansBold24pt7b);
    if (!isnan(indoorTemperature)) {
        snprintf(value, sizeof(value), "%.1f", indoorTemperature);
    } else {
        strcpy(value, "--");
    }
    printTemperature(value, 154, 71, roomColor);

    display.setFont(&fonts::efontCN_16);
    const unsigned long minutes = (millis() - indoorUpdatedMs) / 60000;
    if (!isnan(indoorTemperature)) {
        if (minutes == 0) {
            printAt("刚刚更新", 157, 137, roomColor);
        } else {
            snprintf(value, sizeof(value), "%lu 分钟前", minutes);
            printAt(value, 157, 137, roomColor);
        }
    } else {
        printAt("暂无读数", 157, 137, roomColor);
    }
}

void drawFooter() {
    auto& display = M5.Display;
    display.fillRect(0, 166, 320, 74, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    if (settingsOpen) {
        printAt(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "WiFi 未连接", 12, 174);
        printAt(sdMounted ? "/nuanshi.log" : "SD 未挂载", 185, 174);
        printAt(screenStatus, 12, 195, TFT_LIGHTGREY);
    } else {
        printAt("观测模式", 12, 176, TFT_CYAN);
        printAt(roomState(), 100, 176, roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : TFT_WHITE);
    }
    // Screen coordinates of the three key centers, measured from the panel photo.
    printCentered("-0.5 °C", 72, 220);
    printCentered(settingsOpen ? "返回" : "设置", 160, 220);
    printCentered("+0.5 °C", 249, 220);
}

void refreshClock(bool redraw = false) {
    const time_t now = clockSynced.load() ? time(nullptr) : 0;
    const bool minuteChanged = now / 60 != displayedSecond / 60;
    displayedSecond = now;
    static uint32_t lastRefreshMs = 0;
    if (redraw || minuteChanged || millis() - lastRefreshMs >= 5000) {
        lastRefreshMs = millis();
        drawHeader();
    }
    if (redraw || minuteChanged) {
        drawMeasurements();
        drawFooter();
    }
}

void drawScreen(const char* status) {
    snprintf(screenStatus, sizeof(screenStatus), "%s", status);
    refreshClock(true);
}

void configureLogServer() {
    logServer.on("/", HTTP_GET, []() {
        logServer.send(200, "text/html; charset=utf-8",
                       "<!doctype html><title>NuanShi</title><a href=\"/nuanshi.log\">Download nuanshi.log</a>");
    });
    logServer.on("/nuanshi.log", HTTP_GET, []() {
        File log = openEventLog();
        if (!log) {
            logServer.send(503, "text/plain", "SD log unavailable");
            return;
        }
        logServer.sendHeader("Cache-Control", "no-store");
        logServer.sendHeader("Content-Disposition", "attachment; filename=nuanshi.log");
        logServer.streamFile(log, "text/plain; charset=utf-8");
    });
    logServer.begin();
}

void updateConnectivity() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wifiWasConnected) {
        wifiWasConnected = connected;
        logEvent(connected ? "WiFi link up" : "WiFi link down");
        drawHeader();
    }
    if (connected && clockSynced.load() && !weatherStarted) {
        weatherStarted = startOutdoorReader();
    }
    OutdoorSample sample{};
    if (nextOutdoorSample(sample)) {
        char line[80];
        if (sample.error == 0) {
            outdoor = sample;
            snprintf(line, sizeof(line), "Outdoor Changping temperature=%.1f C source_epoch=%lld",
                     sample.temperature, static_cast<long long>(sample.observedAt));
        } else {
            snprintf(line, sizeof(line), "Outdoor weather request failed code=%d", sample.error);
        }
        Serial.printf("[WEATHER] %s\n", line);
        logEvent(line);
        drawMeasurements();
    }
}

void changeTarget(int adjustment) {
    const int changed = constrain(targetTenths + adjustment, 100, 300);
    if (changed == targetTenths) {
        return;
    }
    targetTenths = changed;
    Preferences preferences;
    bool saved = false;
    if (preferences.begin("nuanshi", false)) {
        saved = preferences.putInt("target", targetTenths) == sizeof(int32_t);
        preferences.end();
    }
    char line[64];
    snprintf(line, sizeof(line), "Target set temperature=%.1f C saved=%s", targetTenths / 10.0f,
             saved ? "yes" : "no");
    Serial.printf("[SET] %s\n", line);
    logEvent(line);
    drawMeasurements();
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
    MijiaSample sample{};
    if (!nextMijiaSample(sample)) {
        return;
    }
    if (sample.failedAttempts) {
        sensorFailures = max(sensorFailures, sample.failedAttempts);
        char line[76];
        snprintf(line, sizeof(line), "Mijia attempt=%u/%u failed code=%d state=%s", sample.failedAttempts,
                 mijiaMaxAttempts, sample.error, roomLost() ? "offline" : "retrying");
        Serial.printf("[MIJIA] %s\n", line);
        logEvent(line);
        drawMeasurements();
        drawFooter();
        return;
    }
    if (mijiaAddressPending) {
        mijiaAddressPending = false;
        Serial.println(saveMijiaAddress(mijiaAddress) ? "[MIJIA] address saved to SD" : "[MIJIA] address active in RAM only");
    }
    if (sensorFailures) {
        logEvent("Mijia recovered");
        readingsInHistory = nextHistoryIndex = 0;
    }
    sensorFailures = 0;
    const MijiaReading& reading = sample.reading;
    indoorTemperature = reading.temperature;
    indoorUpdatedMs = millis();
    temperatureHistory[nextHistoryIndex] = reading.temperature;
    nextHistoryIndex = (nextHistoryIndex + 1) % trendSamples;
    readingsInHistory = min(readingsInHistory + 1, trendSamples);
    char line[140];
    snprintf(line, sizeof(line), "Mijia temperature=%.2f C humidity=%u %% battery=%u mV target=%.1f C outdoor=%.1f C mode=observe trend=%s",
             reading.temperature, reading.humidity, reading.batteryMillivolts, targetTenths / 10.0f,
             hasOutdoorTemperature() ? outdoor.temperature : NAN, roomTrend());
    Serial.printf("[MIJIA] %s\n", line);
    logEvent(line);
    drawMeasurements();
    drawFooter();
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
    Preferences preferences;
    bool targetWasStored = false;
    if (preferences.begin("nuanshi", true)) {
        targetWasStored = preferences.isKey("target");
        targetTenths = constrain(preferences.getInt("target", 220), 100, 300);
        preferences.end();
    }
    pinMode(servoPin, OUTPUT);
    digitalWrite(servoPin, LOW);
    sdMounted = mountStorage();
    Serial.println(sdMounted ? "[SD] mounted" : "[SD] mount failed");
    logEvent("Boot");
    char targetLine[64];
    snprintf(targetLine, sizeof(targetLine), "Target loaded temperature=%.1f C source=%s", targetTenths / 10.0f,
             targetWasStored ? "NVS" : "default");
    Serial.printf("[SET] %s\n", targetLine);
    logEvent(targetLine);
    drawScreen("Ready");
    reportBoard();
    Serial.println("[HELP] ? board, +/- target, w WiFi, b BLE, p PWM bench, n WiFi/SNTP, d SD, t clock, m Mijia");
    WifiCredentials credentials;
    if (loadCredentials(credentials)) {
        Serial.println("[SD] WiFi credentials loaded");
        connectAndSync(credentials, false);
    }
    if (loadMijiaAddress(mijiaAddress) && enableMijia()) {
        Serial.println("[MIJIA] address loaded from SD");
    }
    configureLogServer();
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
    } else if (command == 'w') {
        scanWifi();
    } else if (command == 'b') {
        scanBle();
    } else if (command == 'p') {
        startServoPulse();
    } else if (command == '?') {
        reportBoard();
    } else if (M5.BtnA.wasPressed() || command == '-') {
        changeTarget(-5);
    } else if (M5.BtnC.wasPressed() || command == '+') {
        changeTarget(5);
    } else if (M5.BtnB.wasPressed()) {
        settingsOpen = !settingsOpen;
        drawFooter();
    }
    if (!servoActive) {
        pollMijia();
        updateConnectivity();
        logServer.handleClient();
    }
    refreshClock();
    delay(10);
}
