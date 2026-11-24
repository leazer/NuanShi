#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t mijiaAddressSize = 6;
constexpr int mijiaMinIntervalMinutes = 1;
constexpr int mijiaMaxIntervalMinutes = 20;
constexpr int mijiaDefaultIntervalMinutes = 2;
constexpr unsigned mijiaMaxAttempts = 3;

constexpr bool validMijiaInterval(int minutes) {
    return minutes >= mijiaMinIntervalMinutes && minutes <= mijiaMaxIntervalMinutes;
}

constexpr uint32_t mijiaIntervalMs(int minutes) {
    return static_cast<uint32_t>(minutes) * 60000;
}

constexpr unsigned mijiaHistoryGapSeconds(int minutes) {
    return (minutes > 2 ? minutes + 4 : 6) * 60;
}

int mijiaIntervalMinutes();
void setMijiaIntervalMinutes(int minutes);

struct MijiaReading {
    float temperature;
    unsigned humidity;
    unsigned batteryMillivolts;
};

struct MijiaSample {
    MijiaReading reading;
    unsigned failedAttempts;
    int error;
    uint8_t source; // 0 = primary, 1 = near backup, 2 = far backup.
    bool exhausted;
};

bool startMijiaReader(const uint8_t (&address)[mijiaAddressSize]);
bool nextMijiaSample(MijiaSample& sample);
