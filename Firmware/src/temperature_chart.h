#pragma once

#include <Arduino.h>
#include <time.h>

struct TemperatureRecord;
constexpr uint32_t temperatureHistoryWindow = 86400;
constexpr uint16_t temperatureChartTargetColor = 0xFE2F;
constexpr uint16_t temperatureChartIndoorColor = 0x07FF;

void invalidateTemperatureChart();
int temperatureChartRecords(const TemperatureRecord*& samples);
void drawTemperatureChart(time_t now, int targetTenths);
