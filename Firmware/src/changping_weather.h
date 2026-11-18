#pragma once

#include <time.h>

struct OutdoorSample {
    float temperature;
    time_t observedAt;
    int error;
};

bool startOutdoorReader();
bool nextOutdoorSample(OutdoorSample& sample);
