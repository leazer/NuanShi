#pragma once

#include <stdint.h>

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
};

void startWebPortal(PortalStatus (*readStatus)(), bool (*saveTarget)(int),
                    bool (*saveSchedule)(uint16_t, uint16_t), bool (*saveLowGear)(int));
void pollWebPortal();
