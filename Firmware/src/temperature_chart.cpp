#include "temperature_chart.h"

#include <M5Unified.h>
#include "bench_storage.h"

namespace {
constexpr uint32_t daySeconds = temperatureHistoryWindow;
constexpr uint32_t sixHours = daySeconds / 4;
constexpr int plotLeft = 38;
constexpr int plotRight = 309;
constexpr uint16_t panelColor = 0x10E5;
constexpr uint16_t gridColor = 0x21A9;
constexpr uint16_t mutedColor = 0xA5BA;
constexpr uint16_t roomColor = 0x5736;
constexpr uint16_t weatherColor = 0x857F;
constexpr uint16_t targetColor = 0xFE2F;
// Two-minute indoor samples and half-hour weather updates, with room for reboots.
TemperatureRecord records[1024];
int recordCount = 0;
bool reloadRecords = true;

void label(const char* text, int x, int y, uint16_t color = TFT_WHITE) {
    auto& display = M5.Display;
    display.setTextColor(color);
    display.setCursor(x, y);
    display.print(text);
}

int plotX(uint32_t epoch, uint32_t start) {
    return plotLeft + (epoch - start) * (plotRight - plotLeft) / daySeconds;
}

bool inSeries(const TemperatureRecord& record, bool outdoor, uint32_t start, uint32_t end) {
    const bool correctType = outdoor ? record.targetTenths == TemperatureRecord::outdoor :
                                       record.targetTenths >= 100 && record.targetTenths <= 300;
    return record.epoch >= start && record.epoch <= end &&
           correctType && abs(record.temperatureHundredths) <= 10000;
}

void drawPanel(bool outdoor, uint32_t start, uint32_t end, float latest, uint16_t valueColor, int targetTenths, bool manual) {
    auto& display = M5.Display;
    const int header = outdoor ? (manual ? 119 : 130) : 34;
    const int top = header + 22;
    const int bottom = outdoor ? (manual ? 194 : 215) : (manual ? 105 : 116);
    const uint16_t lineColor = outdoor ? weatherColor : roomColor;
    const uint32_t maxGap = outdoor ? 60 * 60 : 6 * 60;
    display.fillRoundRect(6, header - 3, 308, bottom - header + 8, 4, panelColor);
    display.setFont(&fonts::efontCN_16);
    label(outdoor ? "室外温度 · 昌平" : "室内温度", 13, header);

    char text[24];
    if (isfinite(latest)) {
        snprintf(text, sizeof(text), "%.1f °C", latest);
    } else {
        strcpy(text, "-- °C");
    }
    label(text, plotRight - display.textWidth(text), header, valueColor);
    display.setFont(&fonts::efontCN_12);
    if (!outdoor && !manual) {
        snprintf(text, sizeof(text), "目标 %.1f°C", targetTenths / 10.0f);
        label(text, 101, header + 2, targetColor);
    }

    int low = outdoor ? -200 : manual ? 2300 : min(2300, targetTenths * 10);
    int high = outdoor ? 600 : manual ? 2700 : max(2700, targetTenths * 10);
    for (int i = 0; i < recordCount; ++i) {
        const auto& record = records[i];
        if (inSeries(record, outdoor, start, end)) {
            low = min(low, static_cast<int>(record.temperatureHundredths));
            high = max(high, static_cast<int>(record.temperatureHundredths));
            if (!outdoor && !manual) {
                low = min(low, record.targetTenths * 10);
                high = max(high, record.targetTenths * 10);
            }
        }
    }
    low = static_cast<int>(floorf(low / 100.0f)) * 100;
    const int step = ((high - low + 199) / 200) * 100;
    high = low + 2 * step;
    const auto yAt = [=](int value) {
        return bottom - (value - low) * (bottom - top) / (high - low);
    };
    for (int value = low; value <= high; value += step) {
        const int y = yAt(value);
        display.drawFastHLine(plotLeft, y, plotRight - plotLeft + 1, gridColor);
        snprintf(text, sizeof(text), "%d", value / 100);
        label(text, 32 - display.textWidth(text), y - 6, mutedColor);
    }
    if (end == 0) {
        label("等待校准时间", 120, (top + bottom) / 2 - 6, mutedColor);
        return;
    }
    // Beijing time is UTC+8, including alignment of the six-hour grid.
    constexpr uint32_t timezoneOffset = 8 * 3600;
    for (uint32_t tick = ((start + timezoneOffset) / sixHours + 1) * sixHours - timezoneOffset;
         tick <= end; tick += sixHours) {
        const int x = plotX(tick, start);
        display.drawFastVLine(x, top, bottom - top + 1, gridColor);
        if (outdoor && x >= 65 && x <= 290) {
            snprintf(text, sizeof(text), "%02u:00", static_cast<unsigned>((tick + timezoneOffset) % daySeconds / 3600));
            label(text, x - display.textWidth(text) / 2, manual ? 204 : 225, mutedColor);
        }
    }

    // Draw the recorded target as a step line so changes are visible in history.
    if (!outdoor && !manual) {
        const TemperatureRecord* previous = nullptr;
        for (int i = 0; i < recordCount; ++i) {
            const auto& record = records[i];
            if (!inSeries(record, false, start, end)) {
                continue;
            }
            if (previous && record.epoch > previous->epoch && record.epoch - previous->epoch <= maxGap) {
                const int x = plotX(record.epoch, start);
                const int y = yAt(previous->targetTenths * 10);
                for (int dash = plotX(previous->epoch, start); dash < x; ++dash) {
                    if ((dash - plotLeft) % 7 < 4) display.drawPixel(dash, y, targetColor);
                }
                if (record.targetTenths != previous->targetTenths) {
                    display.drawLine(x, y, x, yAt(record.targetTenths * 10), targetColor);
                }
            }
            previous = &record;
        }
    }

    const TemperatureRecord* previous = nullptr;
    for (int i = 0; i < recordCount; ++i) {
        const auto& record = records[i];
        if (!inSeries(record, outdoor, start, end)) continue;
        const int x = plotX(record.epoch, start);
        const int y = yAt(record.temperatureHundredths);
        if (previous && record.epoch > previous->epoch && record.epoch - previous->epoch <= maxGap) {
            display.drawLine(plotX(previous->epoch, start), yAt(previous->temperatureHundredths), x, y, lineColor);
        }
        if (outdoor) display.fillCircle(x, y, 1, lineColor);
        else display.drawPixel(x, y, lineColor);
        previous = &record;
    }
    if (!previous) {
        label(recordCount < 0 ? "SD 卡不可读" : "暂无历史数据", 120, (top + bottom) / 2 - 6, mutedColor);
    }
}
}  // namespace

void invalidateTemperatureChart() {
    reloadRecords = true;
}

int temperatureChartRecords(const TemperatureRecord*& samples) {
    if (reloadRecords) {
        recordCount = readTemperatureHistory(records, sizeof(records) / sizeof(records[0]));
        reloadRecords = recordCount < 0;
    }
    samples = records;
    return recordCount;
}

void drawTemperatureChart(time_t now, float indoor, uint16_t indoorColor, float outdoor, int targetTenths, bool manual) {
    const uint32_t end = now > daySeconds ? static_cast<uint32_t>(now) : 0;
    if (end) {
        const TemperatureRecord* samples;
        temperatureChartRecords(samples);
    }
    const uint32_t start = end ? end - daySeconds : 0;
    auto& display = M5.Display;
    display.startWrite();
    display.fillRect(0, 31, 320, 209, TFT_BLACK);
    drawPanel(false, start, end, indoor, indoorColor, targetTenths, manual);
    drawPanel(true, start, end, outdoor, weatherColor, targetTenths, manual);
    label("近24h", 9, manual ? 204 : 225, mutedColor);
    display.endWrite();
}
