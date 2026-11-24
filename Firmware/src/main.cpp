#include "lcd_small_font.h"
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
#include "firmware_version.h"
#include "heating_rules_page.h"
#include "jed106.h"
#include "policy_package.h"
#include "target_temperature.h"
#include "temperature_chart.h"
#include "web_portal.h"

namespace {
constexpr uint8_t servoPin = 17;  // M-Bus pin 16, normally UART2 TX.
constexpr uint8_t servoChannel = 4;
constexpr uint8_t servoResolution = 16;
constexpr uint32_t servoFrequency = 50;
constexpr uint32_t servoPeriodUs = 1000000 / servoFrequency;
constexpr uint32_t servoHoldMs = 1500;
constexpr uint16_t servoPulses[] = {1500, 1300, 1700};
constexpr uint32_t indoorStaleGraceMs = 60000;
constexpr uint32_t indoorLostGraceMs = 3 * 60000;
constexpr uint32_t sensorSettlingMs = 30 * 60000UL;
constexpr unsigned trendSamples = 6;  // Roughly ten minutes at the normal sample interval.
constexpr uint32_t menuTimeoutMs = 10000;
constexpr uint32_t messageTimeoutMs = 3000;
constexpr uint32_t backHoldMs = 800;
constexpr uint32_t adjustmentRepeatDelayMs = 400;
constexpr uint32_t adjustmentRepeatIntervalMs = 100;
constexpr int minutesPerDay = 24 * 60;
constexpr int minCalibrationUs = 500;
constexpr int maxCalibrationUs = 2500;
constexpr int calibrationStepUs = 20;

enum class Page {
    Home, Rules, H2s, About, Settings, ControlMode, Target, Schedule, LowGear, SampleInterval, Password, Calibration, ServoTest, Message
};
enum class GearSource { Panel, Web, Test, Automatic };

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
    "控制模式", "目标温度", "采暖时段", "低温加热档位", "温度读取间隔", "舵机标定", "舵机测试", "关于"
};
constexpr Page settingsPages[] = {
    Page::ControlMode, Page::Target, Page::Schedule, Page::LowGear, Page::SampleInterval, Page::Password, Page::ServoTest, Page::About
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

MijiaSensors mijiaSensors;
bool mijiaEnabled = false;
bool mijiaAddressPending = false;
float indoorTemperature = NAN;
uint32_t indoorUpdatedMs = 0;
uint32_t shortenedIntervalAt = 0;
bool awaitingReadAfterShortening = false;
unsigned sensorFailures = 0;
int lastMijiaSource = -1;
HeatingSensorRecovery heatingSensor;
float temperatureHistory[trendSamples] = {};
unsigned readingsInHistory = 0;
unsigned nextHistoryIndex = 0;
int targetTenths = targetMinimumTenths;
HeatingSchedule schedule{19 * 60, 7 * 60};
Calibration calibration{};
int testGear = 0;
int lastGearTenths = 0;
bool manualControl = false;
ControlMode controlMode = ControlMode::Observe;
HeatingPolicy heatingPolicy;
const char* automaticState = "未启用";
bool hasServoMotion = false;
uint32_t lastServoMotion = 0;
struct HeatingSample { uint32_t at; float temperature; };
HeatingSample heatingSamples[42]{};
size_t heatingSampleCount = 0;
size_t heatingSampleNext = 0;
uint32_t lastTemperatureJump = 0;
int lowGearTenths = 10;
Page page = Page::Home;
unsigned rulesPageIndex = 0;
constexpr int h2sHistoryCapacity = 60;
jed106::Readings h2sHistory[h2sHistoryCapacity];
int h2sHistoryCount = 0;
int h2sHistoryNext = 0;
uint32_t h2sSampleAt = 0;
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

bool settingEnabled(Page setting) {
    return !manualControl || (setting != Page::Target && setting != Page::Schedule && setting != Page::LowGear);
}

const char* controlModeLabel(ControlMode mode) {
    switch (mode) {
        case ControlMode::Manual: return "手动控制";
        case ControlMode::Automatic: return "自动控制";
        default: return "观测模式";
    }
}

const char* controlModeName(ControlMode mode) {
    switch (mode) {
        case ControlMode::Manual: return "manual";
        case ControlMode::Automatic: return "auto";
        default: return "observe";
    }
}
bool weatherStarted = false;
bool weatherCacheChecked = false;
bool wifiWasConnected = false;
OutdoorSample outdoor{NAN, 0, -1};

bool hasOutdoorTemperature() {
    return clockSynced.load() && isFreshOutdoorSample(outdoor, time(nullptr));
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
    const char* title = page == Page::Home ? "暖时" : page == Page::Rules ? "控制规则" :
                        page == Page::H2s ? "H2S 读取" : page == Page::About ? "关于" : "设置";
    printAt(title, page == Page::Home ? 20 : 10, 8);
    char date[20] = "--/--  --:--";
    if (displayedSecond > 0) {
        tm local{};
        localtime_r(&displayedSecond, &local);
        strftime(date, sizeof(date), "%m/%d  %H:%M", &local);
    }
    printCentered(date, 160, 8);
    const bool connected = WiFi.status() == WL_CONNECTED;
    const char* mode = manualControl ? "手动" : controlMode == ControlMode::Automatic ? "自动" : "观测";
    printAt(mode, 240, 8, TFT_CYAN);
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

void recordHistory(int16_t value, int16_t tag) {
    if (!sdMounted || !clockSynced.load()) {
        return;
    }
    const TemperatureRecord record{
        static_cast<uint32_t>(time(nullptr)), value, tag
    };
    if (appendTemperatureRecord(record)) {
        invalidateTemperatureChart();
    } else {
        Serial.println("[SD] history write failed");
    }
}

void recordTemperature(float temperature, int16_t recordedTarget) {
    if (isfinite(temperature) && fabsf(temperature) <= 100.0f) {
        recordHistory(lroundf(temperature * 100.0f), recordedTarget);
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
    const bool rescheduling = awaitingReadAfterShortening && millis() - shortenedIntervalAt < indoorStaleGraceMs;
    return sensorFailures >= mijiaMaxAttempts ||
           (!rescheduling && roomHasAged(mijiaIntervalMs(mijiaIntervalMinutes()) + indoorLostGraceMs));
}

bool roomStale() {
    return sensorFailures > 0 || roomHasAged(mijiaIntervalMs(mijiaIntervalMinutes()) + indoorStaleGraceMs);
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

void drawTemperatureSummary() {
    auto& display = M5.Display;
    display.fillRect(240, 31, 80, 185, TFT_BLACK);
    display.drawFastVLine(239, 34, 175, 0x21A9);
    const uint16_t indoorColor = roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : temperatureChartIndoorColor;
    const char* labels[] = {"目标温度", "室内温度", "室外温度"};
    const float temperatures[] = {targetTenths / 10.0f, indoorTemperature,
                                  hasOutdoorTemperature() ? outdoor.temperature : NAN};
    const uint16_t colors[] = {temperatureChartTargetColor, indoorColor, 0x857F};
    constexpr int x = 246;
    for (int i = 0; i < 3; ++i) {
        const int y = 39 + i * 54;
        const int numberY = y + 21;
        display.setFont(&lcdSmallFont);
        printAt(labels[i], x, y, i == 1 ? indoorColor : TFT_LIGHTGREY);
        const int unitWidth = display.textWidth("°C");
        const int unitHeight = display.fontHeight();
        char value[16] = "--";
        if (isfinite(temperatures[i])) snprintf(value, sizeof(value), "%.1f", temperatures[i]);
        display.setFont(&fonts::FreeSansBold12pt7b);
        if (display.textWidth(value) + unitWidth + 3 > 74) display.setFont(&fonts::efontCN_16);
        const int numberWidth = display.textWidth(value);
        const int bottom = numberY + display.fontHeight();
        printAt(value, x, numberY, colors[i]);
        display.setFont(&lcdSmallFont);
        printAt("°C", x + numberWidth + 3, bottom - unitHeight, colors[i]);
    }
}

void drawFooter() {
    if (page != Page::Home) {
        return;
    }
    auto& display = M5.Display;
    display.fillRect(0, 216, 320, 24, TFT_BLACK);
    display.drawFastHLine(8, 216, 304, 0x21A9);
    display.setFont(&fonts::efontCN_16);
    if (manualControl) {
        display.setFont(&lcdSmallFont);
        printAt("手动控制档位", 12, 223, TFT_LIGHTGREY);
        display.setFont(&fonts::efontCN_16);
        printCentered("-", 103, 220);
        printCentered("+", 303, 220);
        for (int gear = 1; gear <= 4; ++gear) {
            const int x = 128 + (gear - 1) * 49;
            const uint16_t background = lastGearTenths == gear * 10 ? 0x035F : TFT_BLACK;
            display.fillRoundRect(x - 13, 219, 27, 19, 4, background);
            display.setTextColor(TFT_WHITE, background);
            display.setCursor(x - display.textWidth("1") / 2, 220);
            display.print(gear);
        }
    } else {
        const char* state = controlMode == ControlMode::Automatic ? automaticState : roomState();
        char status[64];
        if (lastGearTenths) {
            snprintf(status, sizeof(status), "%s   档位: %.1f", state, lastGearTenths / 10.0f);
        } else {
            snprintf(status, sizeof(status), "%s   档位: --", state);
        }
        printCentered(status, 160, 220, roomLost() ? TFT_RED : roomStale() ? TFT_ORANGE : TFT_WHITE);
    }
}

void drawButtonLabels(const char* left, const char* middle, const char* right) {
    M5.Display.setFont(&fonts::efontCN_16);
    // Screen coordinates of the three key centers, measured from the panel photo.
    printCentered(left, 72, 220);
    printCentered(middle, 160, 220);
    printCentered(right, 249, 220);
}

void drawDashboard() {
    if (page != Page::Home) {
        return;
    }
    drawTemperatureChart(clockSynced.load() ? time(nullptr) : 0, targetTenths);
    drawTemperatureSummary();
    drawFooter();
}

void formatTime(char* text, size_t size, unsigned minutes) {
    snprintf(text, size, "%02u:%02u", minutes / 60, minutes % 60);
}

String localIpLabel() {
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("未连接");
}

void drawServoPulseValue(unsigned pulse) {
    auto& display = M5.Display;
    display.fillRect(0, 84, 320, 73, TFT_BLACK);
    char value[32];
    snprintf(value, sizeof(value), "%u us", pulse);
    display.setFont(&fonts::FreeSansBold18pt7b);
    printCentered(value, 160, 87);
    display.setFont(&fonts::efontCN_16);
    snprintf(value, sizeof(value), "占空比 %.3f%%", 100.0 * pulse / servoPeriodUs);
    printCentered(value, 160, 135);
}

void drawCalibrationValue() {
    drawServoPulseValue(editingSecond ? draftCalibration.gearFourUs : draftCalibration.gearOneUs);
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

void drawDraftSampleInterval() {
    auto& display = M5.Display;
    display.fillRect(0, 88, 320, 53, TFT_BLACK);
    char value[12];
    snprintf(value, sizeof(value), "%d", draftValue);
    display.setFont(&fonts::FreeSansBold24pt7b);
    printCentered(value, 160, 91);
    display.setFont(&fonts::efontCN_16);
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

void drawH2sChannel(int top, const char* label, uint16_t color, int32_t jed106::Readings::*channel) {
    auto& display = M5.Display;
    constexpr int left = 43;
    constexpr int right = 207;
    constexpr int height = 61;
    constexpr int valueCenter = 266;
    constexpr uint16_t gridColor = 0x21A9;
    const int bottom = top + height - 1;
    const int first = (h2sHistoryNext + h2sHistoryCapacity - h2sHistoryCount) % h2sHistoryCapacity;

    display.drawRect(left, top, right - left + 1, height, gridColor);
    display.drawFastHLine(left, top + height / 2, right - left + 1, gridColor);
    for (int i = 1; i < 4; ++i) {
        display.drawFastVLine(left + (right - left) * i / 4, top, height, gridColor);
    }

    int32_t low = INT32_MAX;
    int32_t high = -1;
    for (int i = 0; i < h2sHistoryCount; ++i) {
        const auto raw = h2sHistory[(first + i) % h2sHistoryCapacity].*channel;
        if (raw < 0) continue;
        if (raw < low) low = raw;
        if (raw > high) high = raw;
    }

    char value[16];
    if (high >= 0) {
        display.setFont(&lcdSmallFont);
        snprintf(value, sizeof(value), "%ld", static_cast<long>(high));
        printAt(value, 7, high == low ? top + height / 2 - 6 : top, TFT_LIGHTGREY);
        if (high != low) {
            snprintf(value, sizeof(value), "%ld", static_cast<long>(low));
            printAt(value, 7, bottom - 11, TFT_LIGHTGREY);
        }
        int previousX = 0;
        int previousY = -1;
        for (int i = 0; i < h2sHistoryCount; ++i) {
            const auto raw = h2sHistory[(first + i) % h2sHistoryCapacity].*channel;
            if (raw < 0) {
                previousY = -1;
                continue;
            }
            const int slot = h2sHistoryCapacity - h2sHistoryCount + i;
            const int x = left + 1 + (right - left - 2) * slot / (h2sHistoryCapacity - 1);
            const int y = high == low ? top + height / 2 : bottom - 2 - (height - 5) * (raw - low) / (high - low);
            if (previousY >= 0) display.drawLine(previousX, previousY, x, y, color);
            else display.drawPixel(x, y, color);
            previousX = x;
            previousY = y;
        }
    }

    display.setFont(&fonts::efontCN_16);
    printCentered(label, valueCenter, top + 3, color);
    const auto raw = h2sHistoryCount ? h2sHistory[(h2sHistoryNext + h2sHistoryCapacity - 1) % h2sHistoryCapacity].*channel : -1;
    if (raw < 0) {
        printCentered(h2sHistoryCount ? "读取失败" : "等待读取", valueCenter, top + 30,
                      h2sHistoryCount ? TFT_ORANGE : TFT_LIGHTGREY);
    } else {
        display.setFont(&fonts::Font4);
        snprintf(value, sizeof(value), "%ld", static_cast<long>(raw));
        printCentered(value, valueCenter, top + 26, color);
    }
}

void drawH2s() {
    auto& display = M5.Display;
    display.startWrite();
    display.fillRect(0, 33, 320, 181, TFT_BLACK);
    display.setFont(&lcdSmallFont);
    printCentered("0x2A   50 kHz   1 Hz", 125, 37, TFT_LIGHTGREY);
    printCentered("原始值", 266, 37, TFT_LIGHTGREY);
    drawH2sChannel(57, "0xA1", TFT_CYAN, &jed106::Readings::a1);
    drawH2sChannel(132, "0xE1", TFT_YELLOW, &jed106::Readings::e1);
    display.setFont(&lcdSmallFont);
    printAt("-59 s", 43, 198, TFT_LIGHTGREY);
    printAt("现在", 183, 198, TFT_LIGHTGREY);
    display.endWrite();
    drawButtonLabels("返回", "", "");
}

void drawMenu() {
    auto& display = M5.Display;
    display.fillRect(0, 31, 320, 209, TFT_BLACK);
    display.setFont(&fonts::efontCN_16);
    switch (page) {
        case Page::H2s:
            drawH2s();
            break;
        case Page::About: {
            printCentered("暖时", 160, 48, TFT_CYAN);
            char version[40];
            snprintf(version, sizeof(version), "软件版本 %s", firmwareVersion);
            printCentered(version, 160, 76, TFT_LIGHTGREY);
            printCentered("本设备会根据室内外温度，", 160, 105);
            printCentered("自动调节燃气地暖的加热控制。", 160, 128);
            printCentered("WiFi IP", 160, 159, TFT_LIGHTGREY);
            printCentered(localIpLabel().c_str(), 160, 181, TFT_CYAN);
            drawButtonLabels("", "返回", "");
            break;
        }
        case Page::Settings: {
            char start[6], end[6], hours[20], target[16], gear[12], interval[20];
            formatTime(start, sizeof(start), schedule.start);
            formatTime(end, sizeof(end), schedule.end);
            snprintf(hours, sizeof(hours), "%s - %s", start, end);
            snprintf(target, sizeof(target), "%.1f °C", targetTenths / 10.0f);
            snprintf(gear, sizeof(gear), "%.1f", lowGearTenths / 10.0f);
            snprintf(interval, sizeof(interval), "%d 分钟", mijiaIntervalMinutes());
            const char* values[] = {controlModeLabel(controlMode), target, hours, gear, interval,
                                    calibration.valid() ? "已标定" : "未标定", "1-4 档", ""};
            for (int row = 0; row < settingsCount; ++row) {
                const int y = 33 + row * 20;
                const bool selected = row == selectedSetting;
                const uint16_t background = selected ? 0x1148 : TFT_BLACK;
                const uint16_t color = !settingEnabled(settingsPages[row]) ? TFT_DARKGREY :
                                       selected ? TFT_CYAN : TFT_LIGHTGREY;
                display.fillRoundRect(8, y, 304, 19, 4, background);
                display.setTextColor(color, background);
                display.setCursor(17, y + 2);
                display.print(settingsLabels[row]);
                display.setCursor(301 - display.textWidth(values[row]), y + 2);
                display.print(values[row]);
            }
            printCentered("长按 B 返回", 160, 200, TFT_LIGHTGREY);
            drawButtonLabels("上移", "进入", "下移");
            break;
        }
        case Page::ControlMode:
            printCentered("控制模式", 160, 56, TFT_CYAN);
            for (int i = 0; i < 3; ++i) {
                printCentered(controlModeLabel(static_cast<ControlMode>(i)), 160, 93 + i * 30,
                              draftValue == i ? TFT_CYAN : TFT_LIGHTGREY);
            }
            drawButtonLabels("上移", "保存", "下移");
            break;
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
        case Page::SampleInterval:
            printCentered("温度读取间隔", 160, 53, TFT_CYAN);
            drawDraftSampleInterval();
            printCentered("分钟", 160, 146);
            printCentered("1-20 分钟 · 长按 A / C 连调", 160, 195, TFT_LIGHTGREY);
            drawButtonLabels("-1 分", "保存", "+1 分");
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
            drawButtonLabels("-20 us", editingSecond ? "保存" : "下一档", "+20 us");
            break;
        case Page::ServoTest: {
            char title[32] = "舵机测试";
            if (testGear) snprintf(title, sizeof(title), "舵机测试 · %d 档", testGear);
            printCentered(title, 160, 53, TFT_CYAN);
            if (testGear) {
                drawServoPulseValue(calibration.pulseAt(testGear * 10));
            } else {
                printCentered("按 A 或 C 测试 1 档", 160, 104);
            }
            drawButtonLabels("降档", "退出", "升档");
            break;
        }
        case Page::Message:
            display.drawRoundRect(33, 78, 254, 70, 6, TFT_RED);
            printCentered(messageText, 160, 106, TFT_RED);
            break;
        case Page::Rules:
            drawHeatingRulesPage(heatingPolicy, rulesPageIndex, schedule.start, schedule.end, lowGearTenths);
            drawButtonLabels("返回", rulesPageIndex ? "上一页" : "",
                             rulesPageIndex + 1 < heatingRulesPageCount(heatingPolicy) ? "下一页" : "");
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
    if (page == Page::Home) {
        if (redraw || minuteChanged) drawDashboard();
    } else if (redraw || (periodic && (page == Page::About || page == Page::Settings))) {
        drawMenu();
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
        targetTenths, schedule.start, schedule.end, lowGearTenths, sdMounted,
        manualControl, calibration.valid(), lastGearTenths, clockSynced.load() ? static_cast<uint32_t>(time(nullptr)) : 0,
        controlMode, automaticState, heatingPolicy, mijiaIntervalMinutes()
    };
}

void updateConnectivity() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wifiWasConnected) {
        wifiWasConnected = connected;
        logEvent(connected ? "WiFi link up" : "WiFi link down");
        drawHeader();
    }
    if (clockSynced.load() && !weatherCacheChecked) {
        weatherCacheChecked = true;
        if (sdMounted && loadRecentOutdoorSample(outdoor)) {
            Serial.printf("[WEATHER] Restored Changping temperature=%.1f C source_epoch=%lld\n",
                          outdoor.temperature, static_cast<long long>(outdoor.observedAt));
            drawDashboard();
        }
    }
    if (connected && clockSynced.load() && !weatherStarted) {
        weatherStarted = startOutdoorReader();
    }
    OutdoorSample sample{};
    if (nextOutdoorSample(sample)) {
        char line[80];
        if (sample.error == 0) {
            outdoor = sample;
            recordTemperature(sample.temperature, TemperatureRecord::outdoor);
            snprintf(line, sizeof(line), "Outdoor Changping temperature=%.1f C source_epoch=%lld",
                     sample.temperature, static_cast<long long>(sample.observedAt));
        } else {
            snprintf(line, sizeof(line), "Outdoor weather request failed code=%d", sample.error);
        }
        Serial.printf("[WEATHER] %s\n", line);
        logEvent(line);
        drawDashboard();
    }
}

bool saveTarget(int changed) {
    if (!validTargetTemperature(changed)) return false;
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
    drawDashboard();
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
    if (page == Page::Rules) drawMenu();
    return true;
}

bool saveLowGear(int value) {
    if (value == lowGearTenths) return true;
    char event[48];
    snprintf(event, sizeof(event), "Low heating gear=%.1f", value / 10.0f);
    if (!saveSetting("low-gear", value, event)) return false;
    lowGearTenths = value;
    if (page == Page::Rules) drawMenu();
    return true;
}

bool saveMijiaInterval(int minutes) {
    if (!validMijiaInterval(minutes)) return false;
    const int previous = mijiaIntervalMinutes();
    if (minutes == previous) return true;
    char event[48];
    snprintf(event, sizeof(event), "Mijia sample interval=%d min", minutes);
    if (!saveSetting("sample-minutes", minutes, event)) return false;
    if (minutes < previous && roomHasAged(mijiaIntervalMs(minutes) + indoorLostGraceMs)) {
        shortenedIntervalAt = millis();
        awaitingReadAfterShortening = true;
    }
    setMijiaIntervalMinutes(minutes);
    if (page == Page::SampleInterval) draftValue = minutes;
    refreshClock(true);
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
    mijiaEnabled = startMijiaReader(mijiaSensors);
    return mijiaEnabled;
}

void pollMijia() {
    MijiaSample sample{};
    if (!nextMijiaSample(sample)) {
        return;
    }
    constexpr const char* sourceNames[] = {"primary", "near", "far"};
    const char* source = sourceNames[sample.source];
    if (sample.failedAttempts) {
        sensorFailures = max(sensorFailures, sample.exhausted ? mijiaMaxAttempts : 1U);
        if (controlMode == ControlMode::Automatic && roomLost()) heatingSensor.markOffline(millis());
        char line[96];
        snprintf(line, sizeof(line), "Mijia attempt=%u failed code=%d state=%s sensor=%s", sample.failedAttempts,
                 sample.error, roomLost() ? "offline" : "retrying", source);
        Serial.printf("[MIJIA] %s\n", line);
        logEvent(line);
        drawDashboard();
        return;
    }
    if (mijiaAddressPending && sample.source == 0) {
        mijiaAddressPending = false;
        Serial.println(saveMijiaAddress(mijiaSensors.addresses[0]) ? "[MIJIA] address saved to SD" : "[MIJIA] address active in RAM only");
    }
    if (sensorFailures) {
        logEvent("Mijia recovered");
        readingsInHistory = nextHistoryIndex = 0;
    }
    if (controlMode == ControlMode::Automatic && (heatingSensor.offline || roomLost())) {
        heatingSensor.markOffline(millis());
        heatingSensor.markRecovered();
        heatingSampleCount = heatingSampleNext = 0;
    }
    sensorFailures = 0;
    awaitingReadAfterShortening = false;
    const MijiaReading& reading = sample.reading;
    const uint32_t now = millis();
    const bool switched = lastMijiaSource >= 0 && lastMijiaSource != sample.source;
    if (switched) {
        char event[64];
        snprintf(event, sizeof(event), "Mijia source changed from=%s to=%s", sourceNames[lastMijiaSource], source);
        logEvent(event);
        readingsInHistory = nextHistoryIndex = 0;
        heatingSampleCount = heatingSampleNext = 0;
        lastTemperatureJump = 0;
    } else if (isfinite(indoorTemperature) &&
        now - indoorUpdatedMs <= mijiaIntervalMs(mijiaIntervalMinutes()) + indoorLostGraceMs &&
        fabsf(reading.temperature - indoorTemperature) >= 0.6f) {
        heatingSampleCount = heatingSampleNext = 0;
        lastTemperatureJump = now;
    }
    lastMijiaSource = sample.source;
    indoorTemperature = reading.temperature;
    indoorUpdatedMs = now;
    if (lastTemperatureJump && now - lastTemperatureJump >= sensorSettlingMs) lastTemperatureJump = 0;
    if (!lastTemperatureJump) {
        heatingSamples[heatingSampleNext] = {now, reading.temperature};
        heatingSampleNext = (heatingSampleNext + 1) % (sizeof(heatingSamples) / sizeof(heatingSamples[0]));
        heatingSampleCount = min(heatingSampleCount + 1, sizeof(heatingSamples) / sizeof(heatingSamples[0]));
    }
    recordTemperature(reading.temperature, targetTenths);
    temperatureHistory[nextHistoryIndex] = reading.temperature;
    nextHistoryIndex = (nextHistoryIndex + 1) % trendSamples;
    readingsInHistory = min(readingsInHistory + 1, trendSamples);
    char gear[12] = "unknown";
    if (lastGearTenths) snprintf(gear, sizeof(gear), "%.1f", lastGearTenths / 10.0f);
    char line[166];
    snprintf(line, sizeof(line), "Mijia temperature=%.2f C humidity=%u %% battery=%u mV target=%.1f C outdoor=%.1f C mode=%s gear=%s sensor=%s trend=%s",
             reading.temperature, reading.humidity, reading.batteryMillivolts, targetTenths / 10.0f,
             hasOutdoorTemperature() ? outdoor.temperature : NAN, controlModeName(controlMode), gear, source, roomTrend());
    Serial.printf("[MIJIA] %s\n", line);
    logEvent(line);
    drawDashboard();
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
        lastServoMotion = servoStarted;
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
    lastServoMotion = servoStarted;
    hasServoMotion = true;
    servoActive = true;
    servoPreview = preview;
    Serial.printf("[PWM] %sGPIO%u freq=%.2f Hz pulse=%u us duty=%lu\n", preview ? "preview " : "",
                  servoPin, frequency, pulseUs, static_cast<unsigned long>(ledcRead(servoChannel)));
    return true;
}

bool applyGear(int gearTenths, GearSource source, const char* reason = "", unsigned rule = 0) {
    if (!calibration.valid()) return false;
    const int pulse = constrain(calibration.pulseAt(gearTenths), minCalibrationUs, maxCalibrationUs);
    if (!outputServoPulse(pulse, true)) return false;

    lastGearTenths = gearTenths;
    const bool automatic = source == GearSource::Automatic;
    const char* name = source == GearSource::Panel ? "panel" : source == GearSource::Web ? "web" :
                       automatic ? "auto" : "test";
    char line[126];
    if (automatic) {
        snprintf(line, sizeof(line), "Gear command gear=%.1f source=auto pulse=%d us reason=%s revision=%u rule=%u",
                 gearTenths / 10.0f, pulse, reason, heatingPolicy.revision, rule);
    } else {
        snprintf(line, sizeof(line), "Gear command gear=%.1f source=%s pulse=%d us", gearTenths / 10.0f, name, pulse);
    }
    Serial.printf("[GEAR] %s\n", line);
    logEvent(line);
    recordHistory(gearTenths, source == GearSource::Test ? TemperatureRecord::testGear :
                  automatic ? TemperatureRecord::automaticGear : TemperatureRecord::gear);
    drawFooter();
    return true;
}

bool applyWebGear(int gearTenths) {
    return manualControl && (gearTenths == 10 || gearTenths == 20 || gearTenths == 30) &&
           applyGear(gearTenths, GearSource::Web);
}

float hourlyHeatingTrend() {
    for (size_t i = 0; i < heatingSampleCount; ++i) {
        const size_t index = (heatingSampleNext + sizeof(heatingSamples) / sizeof(heatingSamples[0]) - 1 - i) %
                             (sizeof(heatingSamples) / sizeof(heatingSamples[0]));
        const auto& sample = heatingSamples[index];
        const uint32_t age = indoorUpdatedMs - sample.at;
        if (age >= 30 * 60000UL) {
            const unsigned maxAgeMinutes = 30 + max(10, mijiaIntervalMinutes() + 1);
            if (age > maxAgeMinutes * 60000UL) break;
            return (indoorTemperature - sample.temperature) * 3600000.0f / age;
        }
    }
    return 0.0f;
}

void setAutomaticState(const char* state) {
    if (strcmp(automaticState, state) == 0) return;
    automaticState = state;
    drawFooter();
}

void updateAutomaticHeating() {
    if (controlMode != ControlMode::Automatic || servoActive ||
        (page != Page::Home && page != Page::Rules)) return;
    const uint32_t now = millis();
    static uint32_t lastEvaluation = 0;
    if ((!heatingSensor.reevaluate || lastTemperatureJump) && lastEvaluation && now - lastEvaluation < 15000) return;
    lastEvaluation = now;
    if (!calibration.valid() || !validHeatingPolicy(heatingPolicy)) {
        setAutomaticState("等待标定或策略");
        return;
    }
    if (!clockSynced.load()) {
        setAutomaticState("等待校时");
        return;
    }
    const time_t wall = time(nullptr);
    tm local{};
    localtime_r(&wall, &local);
    const bool active = heatingHours(local.tm_hour * 60 + local.tm_min, schedule.start, schedule.end);
    const bool jump = lastTemperatureJump != 0;
    const bool lost = roomLost();
    if (lost) heatingSensor.markOffline(now);
    const bool fallback = heatingSensor.useFallback(
        now, mijiaIntervalMs(mijiaIntervalMinutes()) + indoorLostGraceMs, lastGearTenths);
    const bool fresh = !roomStale() && !jump && isfinite(indoorTemperature);
    const bool recoverNow = active && fresh && heatingSensor.reevaluate;
    const unsigned held = hasServoMotion && !recoverNow ? (now - lastServoMotion) / 60000 : UINT16_MAX;
    const auto decision = decideHeating(heatingPolicy, active, fresh, fallback, indoorTemperature,
                                        hourlyHeatingTrend(), hasOutdoorTemperature() ? outdoor.temperature : NAN,
                                        targetTenths, lowGearTenths, lastGearTenths, held);
    if (decision.gear && decision.gear != lastGearTenths &&
        !applyGear(decision.gear, GearSource::Automatic, decision.reason, decision.rule)) {
        setAutomaticState("档位输出失败");
        heatingSensor.reevaluate = false;
        return;
    }
    if (recoverNow) logEvent("Mijia recovered: heating strategy re-evaluated without hold");
    if (recoverNow || !active) heatingSensor.reevaluate = false;
    setAutomaticState(!active ? "低温时段" : fallback ? "失联低档" : lost ? "失联暂保持" :
                      !fresh ? "等待稳定读数" : "采暖中");
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
    lastGearTenths = 0;
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
    if ((page == Page::Calibration || page == Page::ServoTest) && next != page && servoActive && servoPreview) {
        stopServoPulse();
    }
    if (next == Page::H2s && page != next) {
        h2sHistoryCount = 0;
        h2sHistoryNext = 0;
    }
    page = next;
    if (page == Page::Rules) rulesPageIndex = 0;
    if (page == Page::Home) invalidateTemperatureChart();
    pageActivity = millis();
    refreshClock(true);
}

void pollH2s() {
    const uint32_t now = millis();
    if (h2sHistoryCount && now - h2sSampleAt < 1000) return;
    h2sSampleAt = now;
    const auto readings = jed106::read();
    h2sHistory[h2sHistoryNext] = readings;
    h2sHistoryNext = (h2sHistoryNext + 1) % h2sHistoryCapacity;
    if (h2sHistoryCount < h2sHistoryCapacity) ++h2sHistoryCount;
    Serial.printf("[H2S] %lu ms 50 kHz A1=%ld E1=%ld\n", static_cast<unsigned long>(now),
                  static_cast<long>(readings.a1), static_cast<long>(readings.e1));
    drawH2s();
}

void showMessage(const char* message, Page returnPage) {
    messageText = message;
    messageReturn = returnPage;
    openPage(Page::Message);
}

bool saveHeatingPolicy(const HeatingPolicy& policy) {
    if (!validHeatingPolicy(policy)) return false;
    Preferences preferences;
    if (!preferences.begin("nuanshi", false)) return false;
    const bool saved = preferences.putBytes("heat-rules-v1", &policy, sizeof(policy)) == sizeof(policy);
    preferences.end();
    if (!saved) return false;
    heatingPolicy = policy;
    if (page == Page::Rules) {
        rulesPageIndex = 0;
        drawMenu();
    }
    char event[56];
    snprintf(event, sizeof(event), "Heating policy revision=%u installed rules=%u", policy.revision, policy.count);
    Serial.printf("[SET] %s\n", event);
    logEvent(event);
    return true;
}

bool saveControlMode(ControlMode mode) {
    if (mode == controlMode) return true;
    if (mode == ControlMode::Automatic && (!calibration.valid() || !validHeatingPolicy(heatingPolicy))) return false;
    char event[44];
    snprintf(event, sizeof(event), "Control mode=%s", controlModeName(mode));
    if (!saveSetting("control-mode", static_cast<uint32_t>(mode), event)) return false;

    if (servoActive) stopServoPulse();
    controlMode = mode;
    heatingSensor = {};
    manualControl = mode == ControlMode::Manual;
    automaticState = mode == ControlMode::Automatic ? "等待检查" : "未启用";
    if (page == Page::Home || page == Page::Rules || !settingEnabled(page)) {
        openPage(Page::Home);
    } else {
        if (!settingEnabled(settingsPages[selectedSetting])) selectedSetting = 0;
        if (page == Page::Message && !settingEnabled(messageReturn)) messageReturn = Page::Settings;
        if (page == Page::ControlMode) draftValue = static_cast<int>(controlMode);
        refreshClock(true);
    }
    return true;
}

void openSelectedSetting() {
    const Page next = settingsPages[selectedSetting];
    if (!settingEnabled(next)) return;
    if (next == Page::ServoTest) {
        if (!calibration.valid()) {
            showMessage("请先完成舵机标定", Page::Settings);
            return;
        }
        testGear = 0;
        openPage(next);
        return;
    }
    editingSecond = false;
    passwordIndex = 0;
    passwordMatches = true;
    passwordChord = false;
    draftValue = next == Page::Target ? targetTenths : lowGearTenths;
    draftSchedule = schedule;
    draftCalibration = calibration.valid() ? calibration : Calibration{1300, 1700};
    if (next == Page::ControlMode) draftValue = static_cast<int>(controlMode);
    if (next == Page::SampleInterval) draftValue = mijiaIntervalMinutes();
    openPage(next);
}

int nextManualGear(int current, char key) {
    if (key != 'A' && key != 'C') return current;
    if (!current) return 10;
    const int gear = key == 'A' ? (current + 9) / 10 - 1 : current / 10 + 1;
    return constrain(gear, 1, 4) * 10;
}

void handleMenuKey(char key) {
    char event[100];
    bool saved;
    switch (page) {
        case Page::Home:
            if (manualControl) {
                const int gear = nextManualGear(lastGearTenths, key);
                if (gear == lastGearTenths) return;
                if (!calibration.valid()) {
                    showMessage("请先完成舵机标定", Page::Home);
                } else if (!applyGear(gear, GearSource::Panel)) {
                    showMessage("PWM 输出失败", Page::Home);
                }
            } else if (key == 'A') {
                openPage(Page::Rules);
            }
            return;
        case Page::H2s:
            if (key == 'A') openPage(Page::Home);
            return;
        case Page::Rules:
            if (key == 'A') {
                openPage(Page::Home);
                return;
            }
            if (key == 'B' && rulesPageIndex > 0) --rulesPageIndex;
            else if (key == 'C' && rulesPageIndex + 1 < heatingRulesPageCount(heatingPolicy)) ++rulesPageIndex;
            break;
        case Page::Settings: {
            if (key == 'B') {
                openSelectedSetting();
                return;
            }
            do {
                selectedSetting = (selectedSetting + (key == 'A' ? -1 : 1) + settingsCount) % settingsCount;
            } while (!settingEnabled(settingsPages[selectedSetting]));
            break;
        }
        case Page::ControlMode:
            if (key == 'B') {
                if (draftValue == static_cast<int>(ControlMode::Automatic) && !calibration.valid()) {
                    showMessage("请先完成舵机标定", Page::ControlMode);
                } else if (saveControlMode(static_cast<ControlMode>(draftValue))) openPage(Page::Settings);
                else showMessage("保存失败", Page::ControlMode);
                return;
            }
            draftValue = (draftValue + (key == 'A' ? 2 : 1)) % 3;
            break;
        case Page::Target:
        case Page::LowGear: {
            const bool target = page == Page::Target;
            if (key == 'B') {
                const bool stored = target ? saveTarget(draftValue) : saveLowGear(draftValue);
                if (stored) openPage(Page::Settings);
                else showMessage("保存失败", page);
                return;
            }
            const int step = target ? targetStepTenths : 1;
            const int adjusted = constrain(draftValue + (key == 'A' ? -step : step),
                                           target ? targetMinimumTenths : 5, target ? targetMaximumTenths : 30);
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
        case Page::SampleInterval:
            if (key == 'B') {
                if (saveMijiaInterval(draftValue)) openPage(Page::Settings);
                else showMessage("保存失败", Page::SampleInterval);
            } else {
                const int adjusted = constrain(draftValue + (key == 'A' ? -1 : 1),
                                               mijiaMinIntervalMinutes, mijiaMaxIntervalMinutes);
                if (adjusted != draftValue) {
                    draftValue = adjusted;
                    drawDraftSampleInterval();
                }
            }
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
                lastGearTenths = 0;
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
        case Page::ServoTest: {
            if (key == 'B') {
                openPage(Page::Settings);
                return;
            }
            const int gear = testGear ? constrain(testGear + (key == 'A' ? -1 : 1), 1, 4) : 1;
            if (gear == testGear) return;
            if (!applyGear(gear * 10, GearSource::Test)) {
                showMessage("PWM 输出失败", Page::ServoTest);
                return;
            }
            testGear = gear;
            break;
        }
        case Page::About:
            if (key == 'B') openPage(Page::Settings);
            return;
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
    const bool bHeld = M5.BtnB.pressedFor(backHoldMs);
    const bool bLongRelease = M5.BtnB.wasReleaseFor(backHoldMs);
    const bool primaryPage = page == Page::Home || page == Page::Rules;
    if (!ignoreB && (bHeld || bLongRelease)) {
        suppressBRelease = bHeld;
        if (primaryPage) selectedSetting = 0;
        openPage(primaryPage ? Page::Settings : Page::Home);
        return;
    }
    if (page == Page::Message) {
        if (idle >= messageTimeoutMs) openPage(messageReturn);
        return;
    }
    if (!primaryPage && page != Page::H2s && idle >= menuTimeoutMs) {
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
                            page == Page::LowGear || page == Page::SampleInterval || page == Page::Calibration;
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
    if (!loadBuiltInPolicy(heatingPolicy)) Serial.println("[SET] default heating policy invalid");
    bool targetWasStored = false;
    if (preferences.begin("nuanshi", true)) {
        targetWasStored = preferences.isKey("target");
        targetTenths = constrain(preferences.getInt("target", targetMinimumTenths),
                                 targetMinimumTenths, targetMaximumTenths);
        const uint32_t storedSchedule = preferences.getUInt("schedule", 0xFFFFFFFF);
        const HeatingSchedule loadedSchedule{static_cast<uint16_t>(storedSchedule >> 16),
                                             static_cast<uint16_t>(storedSchedule)};
        if (loadedSchedule.start < minutesPerDay && loadedSchedule.end < minutesPerDay) {
            schedule = loadedSchedule;
        }
        lowGearTenths = constrain(static_cast<int>(preferences.getUInt("low-gear", 10)), 5, 30);
        setMijiaIntervalMinutes(preferences.getUInt("sample-minutes", mijiaDefaultIntervalMinutes));
        const uint32_t defaultMode = preferences.getUInt("manual", 0) == 1 ?
                                     static_cast<uint32_t>(ControlMode::Manual) : 0;
        const uint32_t savedMode = preferences.getUInt("control-mode", defaultMode);
        if (savedMode <= static_cast<uint32_t>(ControlMode::Automatic)) controlMode = static_cast<ControlMode>(savedMode);
        manualControl = controlMode == ControlMode::Manual;
        automaticState = controlMode == ControlMode::Automatic ? "等待检查" : "未启用";
        HeatingPolicy storedPolicy{};
        if (preferences.getBytesLength("heat-rules-v1") == sizeof(storedPolicy) &&
            preferences.getBytes("heat-rules-v1", &storedPolicy, sizeof(storedPolicy)) == sizeof(storedPolicy) &&
            validHeatingPolicy(storedPolicy)) heatingPolicy = storedPolicy;
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
    if (sdMounted && importTemperatureHistory() < 0) {
        Serial.println("[SD] temperature history import failed");
    }
    logEvent("Boot");
    char targetLine[64];
    snprintf(targetLine, sizeof(targetLine), "Target loaded temperature=%.1f C source=%s", targetTenths / 10.0f,
             targetWasStored ? "NVS" : "default");
    Serial.printf("[SET] %s\n", targetLine);
    logEvent(targetLine);
    drawScreen("Ready");
    reportBoard();
    Serial.println("[HELP] ? board, +/- target, w WiFi, b BLE, p PWM bench, n WiFi/SNTP, d SD, t clock, m Mijia");
    DeviceSettings settings;
    loadDeviceSettings(settings);
    if (settings.wifi.ssid[0] || loadCredentials(settings.wifi)) {
        Serial.println("[SD] WiFi credentials loaded");
        connectAndSync(settings.wifi, false);
    }
    mijiaSensors = settings.mijia;
    auto& primaryAddress = mijiaSensors.addresses[0];
    if ((hasMijiaAddress(primaryAddress) || loadMijiaAddress(primaryAddress)) && enableMijia()) {
        Serial.println("[MIJIA] address loaded from SD");
    }
    startWebPortal(readPortalStatus, saveTarget, saveSchedule, saveLowGear, saveControlMode, applyWebGear,
                   saveHeatingPolicy, saveMijiaInterval);
}

void loop() {
    M5.update();
    if (servoActive && millis() - servoStarted >= servoHoldMs) {
        stopServoPulse();
    }
    updateMenu();
    if (page == Page::H2s) pollH2s();
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
            memcpy(mijiaSensors.addresses[0], address, sizeof(address));
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
    } else if (command == 'c' && page == Page::Home) {
        openPage(Page::H2s);
    } else if (command == '?') {
        reportBoard();
    } else if (!manualControl && (command == '-' || command == '+')) {
        saveTarget(constrain(targetTenths + (command == '-' ? -targetStepTenths : targetStepTenths),
                             targetMinimumTenths, targetMaximumTenths));
    }
    if (!servoActive) {
        pollMijia();
        updateConnectivity();
        pollWebPortal();
        updateAutomaticHeating();
    }
    refreshClock();
    delay(10);
}
