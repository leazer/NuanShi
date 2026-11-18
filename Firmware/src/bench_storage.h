#pragma once

#include <Arduino.h>
#include "mijia_ble.h"

struct WifiCredentials {
    char ssid[33] = {};
    char password[65] = {};
};

bool mountStorage();
bool readCredentials(Stream& source, WifiCredentials& credentials);
bool loadCredentials(WifiCredentials& credentials);
bool saveCredentials(const WifiCredentials& credentials);
bool loadMijiaAddress(uint8_t (&address)[mijiaAddressSize]);
bool saveMijiaAddress(const uint8_t (&address)[mijiaAddressSize]);
bool appendLog(const char* event, bool clockSynced, bool verify = false);
