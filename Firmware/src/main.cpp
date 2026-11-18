#include <Arduino.h>
#include <NimBLEDevice.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <esp_sntp.h>
#include <hal/gpio_ll.h>
#include <time.h>

#include "bench_storage.h"
#include "changping_weather.h"
#include "web_portal.h"

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
constexpr uint32_t menuTimeoutMs = 10000;
constexpr uint32_t messageTimeoutMs = 3000;
constexpr uint32_t backHoldMs = 800;
constexpr uint32_t adjustmentRepeatDelayMs = 400;
constexpr uint32_t adjustmentRepeatIntervalMs = 100;
constexpr int minutesPerDay = 24 * 60;
constexpr int minCalibrationUs = 1000;
constexpr int maxCalibrationUs = 2000;
constexpr int calibrationStepUs = 5;

enum class Page { Home, About, Settings, Target, Schedule, LowGear, Wifi, Password, Calibration, Message };

struct HeatingSchedule {
    uint16_t start;
    uint16_t end;
};

struct Calibration {
    uint16_t gearOneUs;
    uint16_t gearFourUs;

    bool valid() const {
        return gearOneUs >= minCalibrationUs && gearOneUs <= maxCalibrationUs &&
               gearFourUs >= minCalibrationUs && gearFourUs <= maxCalibrationUs &&
               gearOneUs != gearFourUs;
    }

    int pulseAt(int gearTenths) const {
        return (gearOneUs * 30 + (gearFourUs - gearOneUs) * (gearTenths - 10) + 15) / 30;
    }
};

constexpr const char* settingsLabels[] = {
    "目标温度", "采暖时段", "低温加热档位", "WiFi IP", "舵机标定"
};
constexpr Page settingsPages[] = {
    Page::Target, Page::Schedule, Page::LowGear, Page::Wifi, Page::Password
};
constexpr int settingsCount = sizeof(settingsPages) / sizeof(settingsPages[0]);

int wifiCount = -1;
int bleCount = -1;
int wifiChannelCount = 0;
bool bleReady = false;
bool servoActive = false;
bool servoPreview = false;
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
HeatingSchedule schedule{19 * 60, 7 * 60};
Calibration calibration{};
int lowGearTenths = 10;
Page page = Page::Home;
Page messageReturn = Page::Settings;
const char* messageText = "";
uint32_t pageActivity = 0;
int selectedSetting = 0;
unsigned passwordIndex = 0;
constexpr char calibrationPassword[] = "ACB";
bool passwordMatches = true;
bool passwordChord = false;
bool suppressBRelease = false;
char repeatingAdjustmentKey = '\0';
uint32_t lastAdjustmentMs = 0;
bool editingSecond = false;
int draftValue = 0;
HeatingSchedule draftSchedule{};
Calibration draftCalibration{};
bool weatherStarted = false;
bool wifiWasConnected = false;
OutdoorSample outdoor{NAN, 0, -1};

bool hasOutdoorTemperature() {
    const double age = difftime(time(nullptr), outdoor.observedAt);
    return clockSynced.load() && isfinite(outdoor.temperature) && age >= -900 && age <= 5400;
}

void printAt(const char* text, int x, int y, uint16_t color = TFT_WHITE) {
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.setCursor(x, y);
    M5.Display.print(text);
}

void printCentered(const char* text, int centerX, int y, uint16_t color = TFT_WHITE) {
    printAt(text, centerX - M5.Display.textWidth(text) / 2, y, color);
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
    const char* title = page == Page::Home ? "暖时" : page == Page::About ? "关于" : "设置";
    printAt(title, 10, 8);
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
    if (page != Page::Home) {
        return;
    }
    auto& display = M5.Display;
    constexpr int leftColumn = 30;
    constexpr int rightColumn = 180;
    display.fillRect(0, 34, 320, 156, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    const int unitWidth = display.textWidth("°C");
    printAt("目标温度", leftColumn, 45, TFT_CYAN);
    printAt("室外温度", leftColumn, 115, TFT_LIGHTGREY);
    const uint16_t roomColor = roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : TFT_WHITE;
    printAt("室内温度", rightColumn, 51, roomColor);

    char value[20];
    display.setFont(&fonts::FreeSansBold18pt7b);
    snprintf(value, sizeof(value), "%.1f", targetTenths / 10.0f);
    printTemperature(value, leftColumn, 68);
    if (hasOutdoorTemperature()) {
        snprintf(value, sizeof(value), "%.1f", outdoor.temperature);
    } else {
        strcpy(value, "--");
    }
    display.setFont(&fonts::FreeSansBold18pt7b);
    printTemperature(value, leftColumn, 138);
    display.setFont(&fonts::FreeSansBold24pt7b);
    if (!isnan(indoorTemperature)) {
        snprintf(value, sizeof(value), "%.1f", indoorTemperature);
    } else {
        strcpy(value, "--");
    }
    const int roomValueX = min(rightColumn, 306 - display.textWidth(value) - 5 - unitWidth);
    printTemperature(value, roomValueX, 82, roomColor);

    display.setFont(&fonts::efontCN_16);
    const unsigned long minutes = (millis() - indoorUpdatedMs) / 60000;
    if (!isnan(indoorTemperature)) {
        if (minutes == 0) {
            printAt("刚刚更新", rightColumn, 151, roomColor);
        } else {
            snprintf(value, sizeof(value), "%lu 分钟前", minutes);
            printAt(value, rightColumn, 151, roomColor);
        }
    } else {
        printAt("暂无读数", rightColumn, 151, roomColor);
    }
}

void drawFooter() {
    if (page != Page::Home) {
        return;
    }
    auto& display = M5.Display;
    display.fillRect(0, 190, 320, 50, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    const char* mode = "观测模式";
    const char* state = roomState();
    const int modeWidth = display.textWidth(mode);
    constexpr int gap = 18;
    constexpr int y = 210;
    const int start = (320 - modeWidth - gap - display.textWidth(state)) / 2;
    printAt(mode, start, y, TFT_CYAN);
    printAt(state, start + modeWidth + gap, y, roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : TFT_WHITE);
}

void drawButtonLabels(const char* left, const char* middle, const char* right) {
    M5.Display.setFont(&fonts::efontCN_16);
    // Screen coordinates of the three key centers, measured from the panel photo.
    printCentered(left, 72, 220);
    printCentered(middle, 160, 220);
    printCentered(right, 249, 220);
}

void formatTime(char* text, size_t size, unsigned minutes) {
    snprintf(text, size, "%02u:%02u", minutes / 60, minutes % 60);
}

String localIpLabel() {
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("未连接");
}

void drawCalibrationValue() {
    auto& display = M5.Display;
    display.fillRect(0, 84, 320, 73, TFT_BLACK);
    const unsigned pulse = editingSecond ? draftCalibration.gearFourUs : draftCalibration.gearOneUs;
    char value[32];
    snprintf(value, sizeof(value), "%u us", pulse);
    display.setFont(&fonts::FreeSansBold18pt7b);
    printCentered(value, 160, 87);
    display.setFont(&fonts::efontCN_16);
    snprintf(value, sizeof(value), "占空比 %.3f%%", 100.0 * pulse / servoPeriodUs);
    printCentered(value, 160, 135);
}

void drawDraftTemperatureOrGear(bool target) {
    auto& display = M5.Display;
    display.fillRect(0, 90, 320, 96, TFT_BLACK);
    char value[32];
    snprintf(value, sizeof(value), "%.1f", draftValue / 10.0f);
    display.setFont(&fonts::FreeSansBold24pt7b);
    if (target) {
        printTemperature(value, 160 - (display.textWidth(value) + 26) / 2, 96);
        return;
    }
    printCentered(value, 160, 96);
    display.setFont(&fonts::efontCN_16);
    if (!calibration.valid()) {
        printCentered("舵机尚未标定", 160, 160, TFT_LIGHTGREY);
        return;
    }
    const int pulse = calibration.pulseAt(draftValue);
    if (pulse >= minCalibrationUs && pulse <= maxCalibrationUs) {
        snprintf(value, sizeof(value), "%d us / %.3f%%", pulse, 100.0 * pulse / servoPeriodUs);
        printCentered(value, 160, 160, TFT_LIGHTGREY);
    } else {
        printCentered("超出标定脉宽范围", 160, 160, TFT_ORANGE);
    }
}

void drawDraftSchedule() {
    auto& display = M5.Display;
    display.fillRect(0, 90, 320, 96, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    char start[6], end[6], value[32];
    formatTime(start, sizeof(start), draftSchedule.start);
    formatTime(end, sizeof(end), draftSchedule.end);
    snprintf(value, sizeof(value), "起始时间  %s", start);
    printCentered(value, 160, 95, editingSecond ? TFT_LIGHTGREY : TFT_CYAN);
    snprintf(value, sizeof(value), "截止时间  %s", end);
    printCentered(value, 160, 127, editingSecond ? TFT_CYAN : TFT_LIGHTGREY);
    if (draftSchedule.end < draftSchedule.start) {
        printCentered("截止时间为次日", 160, 166, TFT_LIGHTGREY);
    }
}

void drawMenu() {
    auto& display = M5.Display;
    display.fillRect(0, 31, 320, 209, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    switch (page) {
        case Page::About:
            printCentered("暖时", 160, 56, TFT_CYAN);
            printCentered("本设备会根据室内外温度，", 160, 96);
            printCentered("自动调节燃气地暖的加热控制。", 160, 122);
            printCentered("当前为观测模式", 160, 171, TFT_LIGHTGREY);
            break;
        case Page::Settings: {
            char start[6], end[6], hours[20], target[16], gear[12];
            formatTime(start, sizeof(start), schedule.start);
            formatTime(end, sizeof(end), schedule.end);
            snprintf(hours, sizeof(hours), "%s - %s", start, end);
            snprintf(target, sizeof(target), "%.1f °C", targetTenths / 10.0f);
            snprintf(gear, sizeof(gear), "%.1f", lowGearTenths / 10.0f);
            const String ip = localIpLabel();
            const char* values[] = {target, hours, gear, ip.c_str(), calibration.valid() ? "已标定" : "未标定"};
            for (int row = 0; row < settingsCount; ++row) {
                const int y = 35 + row * 32;
                const bool selected = row == selectedSetting;
                const uint16_t background = selected ? 0x1148 : TFT_BLACK;
                const uint16_t color = selected ? TFT_CYAN : TFT_LIGHTGREY;
                display.fillRoundRect(8, y, 304, 29, 5, background);
                display.setTextColor(color, background);
                display.setCursor(17, y + 6);
                display.print(settingsLabels[row]);
                display.setCursor(301 - display.textWidth(values[row]), y + 6);
                display.print(values[row]);
            }
            printCentered("长按 B 返回", 160, 200, TFT_LIGHTGREY);
            drawButtonLabels("上移", "进入", "下移");
            break;
        }
        case Page::Target:
        case Page::LowGear: {
            const bool target = page == Page::Target;
            printCentered(target ? "目标温度" : "低温加热档位", 160, 53, TFT_CYAN);
            drawDraftTemperatureOrGear(target);
            printCentered("长按 A / C 连调", 160, 195, TFT_LIGHTGREY);
            drawButtonLabels(target ? "-0.5 °C" : "-0.1", "保存", target ? "+0.5 °C" : "+0.1");
            break;
        }
        case Page::Schedule:
            printCentered("采暖时段", 160, 53, TFT_CYAN);
            drawDraftSchedule();
            printCentered("长按 A / C 连调", 160, 195, TFT_LIGHTGREY);
            drawButtonLabels("-5 分", editingSecond ? "保存" : "下一项", "+5 分");
            break;
        case Page::Wifi:
            printCentered("WiFi IP", 160, 59, TFT_CYAN);
            display.setFont(&fonts::efontCN_24);
            printCentered(localIpLabel().c_str(), 160, 110);
            drawButtonLabels("", "返回", "");
            break;
        case Page::Password:
            printCentered("输入标定密码", 160, 65, TFT_CYAN);
            for (int i = 0; i < 3; ++i) {
                const int x = 130 + i * 30;
                display.drawCircle(x, 120, 5, TFT_CYAN);
                if (i < static_cast<int>(passwordIndex)) {
                    display.fillCircle(x, 120, 5, TFT_CYAN);
                }
            }
            break;
        case Page::Calibration:
            printCentered(editingSecond ? "舵机标定 · 4 档" : "舵机标定 · 1 档", 160, 53, TFT_CYAN);
            drawCalibrationValue();
            printCentered("长按 A / C 连续微调", 160, 168, TFT_LIGHTGREY);
            drawButtonLabels("-5 us", editingSecond ? "保存" : "下一档", "+5 us");
            break;
        case Page::Message:
            display.drawRoundRect(33, 78, 254, 70, 6, TFT_RED);
            printCentered(messageText, 160, 106, TFT_RED);
            break;
        case Page::Home:
            break;
    }
}

void refreshClock(bool redraw = false) {
    const time_t now = clockSynced.load() ? time(nullptr) : 0;
    const bool minuteChanged = now / 60 != displayedSecond / 60;
    displayedSecond = now;
    static uint32_t lastRefreshMs = 0;
    const bool periodic = millis() - lastRefreshMs >= 5000;
    if (redraw || minuteChanged || periodic) {
        lastRefreshMs = millis();
        drawHeader();
    }
    if (page != Page::Home) {
        if (redraw || (periodic && (page == Page::Wifi || page == Page::Settings))) {
            drawMenu();
        }
    } else if (redraw || minuteChanged) {
        drawMeasurements();
        drawFooter();
    }
}

void drawScreen(const char* status) {
    Serial.printf("[STATUS] %s\n", status);
    refreshClock(true);
}

PortalStatus readPortalStatus() {
    return {
        indoorTemperature, hasOutdoorTemperature() ? outdoor.temperature : NAN,
        roomState(), roomLost() ? "danger" : roomStale() ? "warn" : "good",
        (millis() - indoorUpdatedMs) / 60000,
        targetTenths, schedule.start, schedule.end, lowGearTenths, sdMounted
    };
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

bool saveTarget(int changed) {
    if (changed == targetTenths) {
        return true;
    }
    Preferences preferences;
    bool saved = false;
    if (preferences.begin("nuanshi", false)) {
        saved = preferences.putInt("target", changed) == sizeof(int32_t);
        preferences.end();
    }
    if (saved) {
        targetTenths = changed;
    }
    char line[64];
    snprintf(line, sizeof(line), "Target set temperature=%.1f C saved=%s", changed / 10.0f,
             saved ? "yes" : "no");
    Serial.printf("[SET] %s\n", line);
    logEvent(line);
    drawMeasurements();
    return saved;
}

bool saveSetting(const char* key, uint32_t value, const char* event) {
    Preferences preferences;
    bool saved = false;
    if (preferences.begin("nuanshi", false)) {
        saved = preferences.putUInt(key, value) == sizeof(value);
        preferences.end();
    }
    char line[112];
    snprintf(line, sizeof(line), "%s saved=%s", event, saved ? "yes" : "no");
    Serial.printf("[SET] %s\n", line);
    logEvent(line);
    return saved;
}

bool saveSchedule(uint16_t start, uint16_t end) {
    if (start == schedule.start && end == schedule.end) return true;
    char event[72];
    snprintf(event, sizeof(event), "Heating schedule start=%02u:%02u end=%02u:%02u",
             static_cast<unsigned>(start / 60), static_cast<unsigned>(start % 60),
             static_cast<unsigned>(end / 60), static_cast<unsigned>(end % 60));
    if (!saveSetting("schedule", (uint32_t{start} << 16) | end, event)) return false;
    schedule = {start, end};
    return true;
}

bool saveLowGear(int value) {
    if (value == lowGearTenths) return true;
    char event[48];
    snprintf(event, sizeof(event), "Low heating gear=%.1f", value / 10.0f);
    if (!saveSetting("low-gear", value, event)) return false;
    lowGearTenths = value;
    return true;
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

bool outputServoPulse(uint16_t pulseUs, bool preview) {
    const uint32_t duty = (static_cast<uint32_t>(pulseUs) * (1UL << servoResolution) + servoPeriodUs / 2) / servoPeriodUs;
    if (servoActive && servoPreview && preview) {
        ledcWrite(servoChannel, duty);
        servoStarted = millis();
        return true;
    }
    const double frequency = ledcSetup(servoChannel, servoFrequency, servoResolution);
    if (frequency <= 0) {
        Serial.println("[PWM] configuration failed");
        return false;
    }
    ledcAttachPin(servoPin, servoChannel);
    ledcWrite(servoChannel, duty);
    servoStarted = millis();
    servoActive = true;
    servoPreview = preview;
    Serial.printf("[PWM] %sGPIO%u freq=%.2f Hz pulse=%u us duty=%lu\n", preview ? "calibration " : "",
                  servoPin, frequency, pulseUs, static_cast<unsigned long>(ledcRead(servoChannel)));
    return true;
}

void stopServoPulse() {
    ledcWrite(servoChannel, 0);
    ledcDetachPin(servoPin);
    pinMode(servoPin, OUTPUT);
    digitalWrite(servoPin, LOW);
    servoActive = false;
    Serial.println("[PWM] output stopped");
    if (!servoPreview) {
        logEvent("PWM test completed");
    }
}

void startServoPulse() {
    const uint16_t pulseUs = servoPulses[nextPulse];
    if (!outputServoPulse(pulseUs, false)) {
        drawScreen("PWM setup failed");
        return;
    }
    nextPulse = (nextPulse + 1) % (sizeof(servoPulses) / sizeof(servoPulses[0]));
    // Enable pad readback without changing the GPIO matrix output from LEDC.
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(servoPin));
    const unsigned long highUs = pulseIn(servoPin, HIGH, 60000);
    const unsigned long lowUs = pulseIn(servoPin, LOW, 60000);
    Serial.printf("[PWM] pad high=%lu us low=%lu us\n", highUs, lowUs);
    char status[32];
    snprintf(status, sizeof(status), "PWM GPIO%u: %u us", servoPin, pulseUs);
    drawScreen(status);
}

void openPage(Page next) {
    if (page == Page::Calibration && next != page && servoActive && servoPreview) {
        stopServoPulse();
    }
    page = next;
    pageActivity = millis();
    refreshClock(true);
}

void showMessage(const char* message, Page returnPage) {
    messageText = message;
    messageReturn = returnPage;
    openPage(Page::Message);
}

void openSelectedSetting() {
    const Page next = settingsPages[selectedSetting];
    editingSecond = false;
    passwordIndex = 0;
    passwordMatches = true;
    passwordChord = false;
    draftValue = next == Page::Target ? targetTenths : lowGearTenths;
    draftSchedule = schedule;
    draftCalibration = calibration.valid() ? calibration : Calibration{1300, 1700};
    openPage(next);
}

void handleMenuKey(char key) {
    char event[100];
    bool saved;
    switch (page) {
        case Page::Home:
            if (key == 'B') {
                selectedSetting = 0;
                openPage(Page::Settings);
            } else {
                openPage(Page::About);
            }
            return;
        case Page::Settings: {
            if (key == 'B') {
                openSelectedSetting();
                return;
            }
            selectedSetting = (selectedSetting + (key == 'A' ? -1 : 1) + settingsCount) % settingsCount;
            break;
        }
        case Page::Target:
        case Page::LowGear: {
            const bool target = page == Page::Target;
            if (key == 'B') {
                const bool stored = target ? saveTarget(draftValue) : saveLowGear(draftValue);
                if (stored) openPage(Page::Settings);
                else showMessage("保存失败", page);
                return;
            }
            const int step = target ? 5 : 1;
            const int adjusted = constrain(draftValue + (key == 'A' ? -step : step), target ? 100 : 5, target ? 300 : 30);
            if (adjusted != draftValue) {
                draftValue = adjusted;
                drawDraftTemperatureOrGear(target);
            }
            return;
        }
        case Page::Schedule:
            if (key != 'B') {
                auto& minutes = editingSecond ? draftSchedule.end : draftSchedule.start;
                minutes = (minutes + (key == 'A' ? -5 : 5) + minutesPerDay) % minutesPerDay;
                drawDraftSchedule();
                return;
            } else if (!editingSecond) {
                editingSecond = true;
            } else {
                if (saveSchedule(draftSchedule.start, draftSchedule.end)) openPage(Page::Settings);
                else showMessage("保存失败", Page::Schedule);
                return;
            }
            break;
        case Page::Wifi:
            if (key == 'B') openPage(Page::Settings);
            return;
        case Page::Password:
            passwordMatches &= key == calibrationPassword[passwordIndex];
            if (++passwordIndex == sizeof(calibrationPassword) - 1) {
                if (passwordMatches) openPage(Page::Calibration);
                else showMessage("密码错误", Page::Settings);
                return;
            }
            break;
        case Page::Calibration:
            if (key != 'B') {
                auto& pulse = editingSecond ? draftCalibration.gearFourUs : draftCalibration.gearOneUs;
                const int adjusted = constrain(pulse + (key == 'A' ? -calibrationStepUs : calibrationStepUs),
                                               minCalibrationUs, maxCalibrationUs);
                if (adjusted == pulse) return;
                if (!outputServoPulse(adjusted, true)) {
                    showMessage("PWM 输出失败", Page::Calibration);
                    return;
                }
                pulse = adjusted;
                drawCalibrationValue();
                return;
            } else if (!editingSecond) {
                if (servoActive && servoPreview) stopServoPulse();
                editingSecond = true;
            } else if (!draftCalibration.valid()) {
                showMessage("两档脉宽不能相同", Page::Calibration);
                return;
            } else {
                snprintf(event, sizeof(event), "Servo calibration gear1=%d gear2=%d gear3=%d gear4=%d us",
                         draftCalibration.pulseAt(10), draftCalibration.pulseAt(20),
                         draftCalibration.pulseAt(30), draftCalibration.pulseAt(40));
                saved = saveSetting("calibration", (uint32_t{draftCalibration.gearOneUs} << 16) |
                                    draftCalibration.gearFourUs, event);
                if (saved) {
                    calibration = draftCalibration;
                    openPage(Page::Settings);
                } else {
                    showMessage("保存失败", Page::Calibration);
                }
                return;
            }
            break;
        case Page::About:
        case Page::Message:
            return;
    }
    drawMenu();
}

void updateMenu() {
    const uint32_t now = millis();
    const uint32_t idle = now - pageActivity;
    const bool bReleased = M5.BtnB.wasReleased();
    const bool ignoreB = suppressBRelease;
    const bool aReleased = M5.BtnA.wasReleased();
    const bool cReleased = M5.BtnC.wasReleased();
    const bool repeatedA = aReleased && repeatingAdjustmentKey == 'A';
    const bool repeatedC = cReleased && repeatingAdjustmentKey == 'C';
    if (repeatedA || repeatedC) {
        repeatingAdjustmentKey = '\0';
    }
    if (bReleased) {
        suppressBRelease = false;
    }
    if (page == Page::Message) {
        if (idle >= messageTimeoutMs) openPage(messageReturn);
        return;
    }
    if (page == Page::About) {
        if (idle >= menuTimeoutMs) openPage(Page::Home);
        return;
    }
    const bool bHeld = M5.BtnB.pressedFor(backHoldMs);
    const bool bLongRelease = M5.BtnB.wasReleaseFor(backHoldMs);
    if (page != Page::Home && !ignoreB && (bHeld || bLongRelease)) {
        suppressBRelease = bHeld;
        openPage(Page::Home);
        return;
    }
    if (page != Page::Home && idle >= menuTimeoutMs) {
        openPage(Page::Home);
        return;
    }
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
        pageActivity = now;
    }
    const bool aDown = M5.BtnA.isPressed();
    const bool cDown = M5.BtnC.isPressed();
    const bool bDown = M5.BtnB.isPressed();
    if (page == Page::Password) {
        const int downCount = aDown + bDown + cDown;
        if (downCount > 1) passwordChord = true;
        if (passwordChord) {
            passwordChord = downCount != 0;
            return;
        }
    }
    const bool adjustable = page == Page::Target || page == Page::Schedule ||
                            page == Page::LowGear || page == Page::Calibration;
    if (adjustable && (aDown || cDown)) {
        if (aDown != cDown && !bDown) {
            const char key = aDown ? 'A' : 'C';
            const auto& button = aDown ? M5.BtnA : M5.BtnC;
            const bool firstRepeat = repeatingAdjustmentKey == '\0' && button.pressedFor(adjustmentRepeatDelayMs);
            const bool nextRepeat = repeatingAdjustmentKey == key && now - lastAdjustmentMs >= adjustmentRepeatIntervalMs;
            if (firstRepeat || nextRepeat) {
                repeatingAdjustmentKey = key;
                lastAdjustmentMs = now;
                handleMenuKey(key);
                pageActivity = millis();
            }
        }
        return;
    }
    if (aReleased && !repeatedA) {
        handleMenuKey('A');
    } else if (bReleased && !ignoreB && !bLongRelease) {
        handleMenuKey('B');
    } else if (cReleased && !repeatedC) {
        handleMenuKey('C');
    } else {
        return;
    }
    pageActivity = millis();
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
        const uint32_t storedSchedule = preferences.getUInt("schedule", 0xFFFFFFFF);
        const HeatingSchedule loadedSchedule{static_cast<uint16_t>(storedSchedule >> 16),
                                             static_cast<uint16_t>(storedSchedule)};
        if (loadedSchedule.start < minutesPerDay && loadedSchedule.end < minutesPerDay) {
            schedule = loadedSchedule;
        }
        lowGearTenths = constrain(static_cast<int>(preferences.getUInt("low-gear", 10)), 5, 30);
        const uint32_t storedCalibration = preferences.getUInt("calibration", 0);
        const Calibration loadedCalibration{static_cast<uint16_t>(storedCalibration >> 16),
                                            static_cast<uint16_t>(storedCalibration)};
        if (loadedCalibration.valid()) calibration = loadedCalibration;
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
    startWebPortal(readPortalStatus, saveTarget, saveSchedule, saveLowGear);
}

void loop() {
    M5.update();
    if (servoActive && millis() - servoStarted >= servoHoldMs) {
        stopServoPulse();
    }
    updateMenu();
    const int command = servoActive ? -1 : Serial.read();
    if (command == 'n') {
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
    } else if (command == 'p' && page == Page::Home) {
        startServoPulse();
    } else if (command == '?') {
        reportBoard();
    } else if (command == '-' || command == '+') {
        saveTarget(constrain(targetTenths + (command == '-' ? -5 : 5), 100, 300));
    }
    if (!servoActive) {
        pollMijia();
        updateConnectivity();
        pollWebPortal();
    }
    refreshClock();
    delay(10);
}
