#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t mijiaAddressSize = 6;

struct MijiaReading {
    float temperature;
    unsigned humidity;
    unsigned batteryMillivolts;
};

struct MijiaSample {
    MijiaReading reading;
    unsigned failedAttempts;
    int error;
};

bool startMijiaReader(const uint8_t (&address)[mijiaAddressSize]);
bool nextMijiaSample(MijiaSample& sample);
