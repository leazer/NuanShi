#pragma once

#include "mijia_ble.h"

struct WifiCredentials {
    char ssid[33] = {};
    char password[65] = {};
};

struct DeviceSettings {
    WifiCredentials wifi;
    MijiaSensors mijia;
};

bool parseDeviceSettings(const char* data, size_t size, DeviceSettings& settings);
