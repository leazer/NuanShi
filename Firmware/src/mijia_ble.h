#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t mijiaAddressSize = 6;

struct MijiaReading {
    float temperature;
    unsigned humidity;
    unsigned batteryMillivolts;
};

bool startMijiaReader(const uint8_t (&address)[mijiaAddressSize]);
bool nextMijiaReading(MijiaReading& reading);
