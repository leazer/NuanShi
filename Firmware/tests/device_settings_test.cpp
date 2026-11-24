#include "device_settings.h"

#include <assert.h>
#include <string.h>

int main() {
    DeviceSettings settings;
    const char complete[] = R"({"wifi":{"ssid":"示例","password":""},"mijia":{"primary":"02:00:00:00:00:01","backup_near":"02:00:00:00:00:0a","backup_far":"02:00:00:00:00:FF"}})";
    assert(parseDeviceSettings(complete, sizeof(complete) - 1, settings));
    assert(strcmp(settings.wifi.ssid, "示例") == 0);
    assert(settings.wifi.password[0] == '\0');
    assert(settings.mijia.addresses[0][0] == 2 && settings.mijia.addresses[0][5] == 1);
    assert(settings.mijia.addresses[1][5] == 10 && settings.mijia.addresses[2][5] == 255);

    const char backupsOnly[] = R"({"mijia":{"backup_far":"02:00:00:00:00:03"}})";
    assert(parseDeviceSettings(backupsOnly, sizeof(backupsOnly) - 1, settings));
    assert(!settings.wifi.ssid[0] && !hasMijiaAddress(settings.mijia.addresses[0]));
    assert(!hasMijiaAddress(settings.mijia.addresses[1]) && settings.mijia.addresses[2][5] == 3);

    const char invalidFields[] = R"({"wifi":{"ssid":"bad\u0000ssid","password":"test"},"mijia":{"primary":"02:00:00:00:00:04","backup_near":"02:00:00:00:00:GG","backup_far":"02:00:00:00:00:05"}})";
    assert(parseDeviceSettings(invalidFields, sizeof(invalidFields) - 1, settings));
    assert(!settings.wifi.ssid[0]);
    assert(settings.mijia.addresses[0][5] == 4 && !hasMijiaAddress(settings.mijia.addresses[1]));
    assert(settings.mijia.addresses[2][5] == 5);

    assert(!parseDeviceSettings("{", 1, settings));
    assert(!parseDeviceSettings("[]", 2, settings));
    assert(!hasMijiaAddress(settings.mijia.addresses[0]));
}
