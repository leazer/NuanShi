#include "lcd_small_font.h"
#include "temperature_chart.h"

#include <M5Unified.h>
#include "bench_storage.h"

namespace {
constexpr uint32_t daySeconds = temperatureHistoryWindow;
constexpr uint32_t sixHours = daySeconds / 4;
constexpr int plotLeft = 38;
constexpr int plotRight = 225;
constexpr uint16_t panelColor = 0x10E5;
constexpr uint16_t gridColor = 0x21A9;
constexpr uint16_t mutedColor = 0xA5BA;
constexpr uint16_t weatherColor = 0x857F;
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

bool inSeries(const TemperatureRecord& record, bool outdoor, uint32_t start, uint32_t end) {
    const bool correctType = outdoor ? record.targetTenths == TemperatureRecord::outdoor :
                                       record.targetTenths >= 100 && record.targetTenths <= 300;
    return record.epoch >= start && record.epoch <= end &&
           correctType && abs(record.temperatureHundredths) <= 10000;
}

void drawPanel(bool outdoor, uint32_t start, uint32_t end, int targetTenths) {
    auto& display = M5.Display;
    const auto xAt = [=](uint32_t epoch) {
        return plotLeft + (epoch - start) * (plotRight - plotLeft) / daySeconds;
    };
    const int header = outdoor ? 119 : 34;
    const int top = header + 18;
    const int bottom = outdoor ? 194 : 105;
    const uint16_t lineColor = outdoor ? weatherColor : temperatureChartIndoorColor;
    const uint32_t maxGap = outdoor ? 60 * 60 : mijiaHistoryGapSeconds(mijiaIntervalMinutes());
    display.fillRoundRect(6, header - 3, 227, bottom - header + 8, 4, panelColor);
    display.setFont(&lcdSmallFont);
    label(outdoor ? "室外温度 昌平" : "室内温度", 13, header,
          outdoor ? TFT_WHITE : temperatureChartIndoorColor);

    char text[24];
    int low = outdoor ? -200 : min(2300, targetTenths * 10);
    int high = outdoor ? 600 : max(2700, targetTenths * 10);
    for (int i = 0; i < recordCount; ++i) {
        const auto& record = records[i];
        if (inSeries(record, outdoor, start, end)) {
            low = min(low, static_cast<int>(record.temperatureHundredths));
            high = max(high, static_cast<int>(record.temperatureHundredths));
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
        const char* waiting = "等待校准时间";
        label(waiting, (plotLeft + plotRight - display.textWidth(waiting)) / 2, (top + bottom) / 2 - 6, mutedColor);
        return;
    }
    // Beijing time is UTC+8, including alignment of the six-hour grid.
    constexpr uint32_t timezoneOffset = 8 * 3600;
    for (uint32_t tick = ((start + timezoneOffset) / sixHours + 1) * sixHours - timezoneOffset;
         tick <= end; tick += sixHours) {
        const int x = xAt(tick);
        display.drawFastVLine(x, top, bottom - top + 1, gridColor);
        if (outdoor && x >= 65 && x <= plotRight - 17) {
            snprintf(text, sizeof(text), "%02u:00", static_cast<unsigned>((tick + timezoneOffset) % daySeconds / 3600));
            label(text, x - display.textWidth(text) / 2, 203, mutedColor);
        }
    }

    if (!outdoor) {
        const int y = yAt(targetTenths * 10);
        for (int x = plotLeft; x <= plotRight; x += 7) {
            display.drawFastHLine(x, y, min(4, plotRight - x + 1), temperatureChartTargetColor);
        }
    }

    const TemperatureRecord* previous = nullptr;
    for (int i = 0; i < recordCount; ++i) {
        const auto& record = records[i];
        if (!inSeries(record, outdoor, start, end)) continue;
        const int x = xAt(record.epoch);
        const int y = yAt(record.temperatureHundredths);
        if (previous && record.epoch > previous->epoch && record.epoch - previous->epoch <= maxGap) {
            display.drawLine(xAt(previous->epoch), yAt(previous->temperatureHundredths), x, y, lineColor);
        }
        if (outdoor) display.fillCircle(x, y, 1, lineColor);
        else display.drawPixel(x, y, lineColor);
        previous = &record;
    }
    if (!previous) {
        const char* state = recordCount < 0 ? "SD 卡不可读" : "暂无历史数据";
        label(state, (plotLeft + plotRight - display.textWidth(state)) / 2, (top + bottom) / 2 - 6, mutedColor);
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

void drawTemperatureChart(time_t now, int targetTenths) {
    const uint32_t end = now > daySeconds ? static_cast<uint32_t>(now) : 0;
    if (end) {
        const TemperatureRecord* samples;
        temperatureChartRecords(samples);
    }
    const uint32_t start = end ? end - daySeconds : 0;
    auto& display = M5.Display;
    display.startWrite();
    display.fillRect(0, 31, 240, 185, TFT_BLACK);
    drawPanel(false, start, end, targetTenths);
    drawPanel(true, start, end, targetTenths);
    label("近24h", 9, 203, mutedColor);
    display.endWrite();
}
