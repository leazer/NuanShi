#pragma once

constexpr int targetMinimumTenths = 230;
constexpr int targetMaximumTenths = 260;
constexpr int targetStepTenths = 5;

constexpr bool validTargetTemperature(int tenths) {
    return tenths >= targetMinimumTenths && tenths <= targetMaximumTenths && tenths % targetStepTenths == 0;
}
