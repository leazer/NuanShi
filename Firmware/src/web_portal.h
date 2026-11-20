#pragma once

#include <stdint.h>
#include "heating_policy.h"

struct PortalStatus {
    float indoor;
    float outdoor;
    const char* roomState;
    const char* roomTone;
    unsigned long indoorAgeMinutes;
    int targetTenths;
    uint16_t heatingStart;
    uint16_t heatingEnd;
    int lowGearTenths;
    bool sdMounted;
    bool manual;
    bool calibrated;
    int gearTenths;
    uint32_t epoch;
    ControlMode mode;
    const char* autoState;
    HeatingPolicy policy;
    int sampleIntervalMinutes;
};

void startWebPortal(PortalStatus (*readStatus)(), bool (*saveTarget)(int),
                    bool (*saveSchedule)(uint16_t, uint16_t), bool (*saveLowGear)(int),
                    bool (*saveMode)(ControlMode), bool (*applyGear)(int),
                    bool (*savePolicy)(const HeatingPolicy&), bool (*saveSampleInterval)(int));
void pollWebPortal();
