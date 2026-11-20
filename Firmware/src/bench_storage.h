#pragma once

#include <Arduino.h>
#include <FS.h>
#include "mijia_ble.h"

struct WifiCredentials {
    char ssid[33] = {};
    char password[65] = {};
};

struct TemperatureRecord {
    static constexpr int16_t outdoor = INT16_MIN;
    static constexpr int16_t gear = INT16_MIN + 1;
    static constexpr int16_t testGear = INT16_MIN + 2;
    static constexpr int16_t automaticGear = INT16_MIN + 3;
    uint32_t epoch;
    int16_t temperatureHundredths;  // Gear in tenths for gear records.
    int16_t targetTenths;  // A negative tag identifies other record types.
};
static_assert(sizeof(TemperatureRecord) == 8, "Keep the SD record format fixed");

bool mountStorage();
bool readCredentials(Stream& source, WifiCredentials& credentials);
bool loadCredentials(WifiCredentials& credentials);
bool saveCredentials(const WifiCredentials& credentials);
bool loadMijiaAddress(uint8_t (&address)[mijiaAddressSize]);
bool saveMijiaAddress(const uint8_t (&address)[mijiaAddressSize]);
bool appendLog(const char* event, bool clockSynced, bool verify = false);
File openEventLog();
bool deleteEventLog();
bool appendTemperatureRecord(const TemperatureRecord& record);
int importTemperatureHistory();
// Returns the number of complete records, or -1 on an SD error.
int readTemperatureHistory(TemperatureRecord* records, size_t capacity);
