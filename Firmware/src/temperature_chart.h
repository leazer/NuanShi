#pragma once

#include <Arduino.h>
#include <time.h>

struct TemperatureRecord;
constexpr uint32_t temperatureHistoryWindow = 86400;

void invalidateTemperatureChart();
int temperatureChartRecords(const TemperatureRecord*& samples);
void drawTemperatureChart(time_t now, float indoor, uint16_t indoorColor, float outdoor, int targetTenths, bool manual);
