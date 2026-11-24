#include "device_settings.h"

#include <ArduinoJson.h>
#include <string.h>

namespace {
int hexDigit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

void readMijiaAddress(JsonString mac, uint8_t (&address)[mijiaAddressSize]) {
    if (mac.size() != 17) return;
    uint8_t parsed[mijiaAddressSize];
    for (size_t i = 0; i < mijiaAddressSize; ++i) {
        const char* octet = mac.c_str() + 3 * i;
        const int high = hexDigit(octet[0]);
        const int low = hexDigit(octet[1]);
        if (high < 0 || low < 0 || (i + 1 < mijiaAddressSize && octet[2] != ':')) return;
        parsed[i] = static_cast<uint8_t>((high << 4) | low);
    }
    memcpy(address, parsed, sizeof(address));
}

void readWifi(JsonObjectConst wifi, WifiCredentials& credentials) {
    const JsonString ssid = wifi["ssid"].as<JsonString>();
    const JsonString password = wifi["password"].as<JsonString>();
    if (ssid.size() == 0 || ssid.size() >= sizeof(credentials.ssid) ||
        password.isNull() || password.size() >= sizeof(credentials.password) ||
        strlen(ssid.c_str()) != ssid.size() || strlen(password.c_str()) != password.size()) return;
    memcpy(credentials.ssid, ssid.c_str(), ssid.size());
    memcpy(credentials.password, password.c_str(), password.size());
}
}  // namespace

bool parseDeviceSettings(const char* data, size_t size, DeviceSettings& settings) {
    settings = {};
    JsonDocument document;
    if (deserializeJson(document, data, size) || !document.is<JsonObjectConst>()) return false;

    readWifi(document["wifi"].as<JsonObjectConst>(), settings.wifi);
    const JsonObjectConst mijia = document["mijia"].as<JsonObjectConst>();
    readMijiaAddress(mijia["primary"].as<JsonString>(), settings.mijia.addresses[0]);
    readMijiaAddress(mijia["backup_near"].as<JsonString>(), settings.mijia.addresses[1]);
    readMijiaAddress(mijia["backup_far"].as<JsonString>(), settings.mijia.addresses[2]);
    return true;
}
