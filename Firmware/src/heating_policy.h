#pragma once

#include <stdint.h>
#include <math.h>

enum class ControlMode : uint8_t { Observe, Manual, Automatic };
enum class HeatingMetric : uint8_t { ForecastDeficit, IndoorDeficit, Trend, Outdoor, Gear };

struct HeatingSensorRecovery {
    bool offline = false;
    bool reevaluate = false;
    uint32_t offlineSince = 0;

    void markOffline(uint32_t now) {
        if (!offline) offlineSince = now;
        offline = true;
        reevaluate = false;
    }

    void markRecovered() {
        if (offline) reevaluate = true;
        offline = false;
    }

    bool useFallback(uint32_t now, uint32_t grace, int currentGear) const {
        return offline && (!currentGear || now - offlineSince >= grace);
    }
};

constexpr int keepGear = 0;
constexpr int useLowGear = -1;
constexpr unsigned maxHeatingRules = 8;
constexpr unsigned maxHeatingConditions = 3;

struct HeatingCondition {
    HeatingMetric metric;
    bool atLeast;
    int16_t thresholdTenths;
};

struct HeatingRule {
    int16_t gear = keepGear;
    uint8_t count = 0;
    HeatingCondition conditions[maxHeatingConditions]{};
};

struct HeatingPolicy {
    uint16_t revision = 1;
    uint16_t holdMinutes = 90;
    uint16_t forecastMinutes = 60;
    uint8_t count = 0;
    HeatingRule rules[maxHeatingRules]{};
};

inline bool validHeatingPolicy(const HeatingPolicy& policy) {
    if (!policy.revision || policy.holdMinutes < 60 || policy.holdMinutes > 180 ||
        policy.forecastMinutes > 90 || !policy.count || policy.count > maxHeatingRules) return false;
    for (unsigned i = 0; i < policy.count; ++i) {
        const auto& rule = policy.rules[i];
        if (!rule.count || rule.count > maxHeatingConditions) return false;
        if (rule.gear != keepGear && rule.gear != useLowGear && (rule.gear < 5 || rule.gear > 40)) return false;
        for (unsigned j = 0; j < rule.count; ++j) {
            const auto& condition = rule.conditions[j];
            if (condition.metric > HeatingMetric::Gear ||
                condition.thresholdTenths < -650 || condition.thresholdTenths > 650) return false;
        }
    }
    return true;
}

struct HeatingDecision {
    int gear;  // Zero keeps the last command.
    const char* reason;
    unsigned rule;

    HeatingDecision(int gear, const char* reason, unsigned rule = 0)
        : gear(gear), reason(reason), rule(rule) {}
};

inline bool heatingHours(unsigned minute, unsigned start, unsigned end) {
    if (start < end) return minute >= start && minute < end;
    return start > end && (minute >= start || minute < end);
}

inline HeatingDecision decideHeating(const HeatingPolicy& policy, bool inHours, bool fresh, bool lost,
                                    float indoor, float hourlyTrend, float outdoor, int targetTenths,
                                    int lowGear, int lastGear, unsigned heldMinutes) {
    if (!inHours) return {lowGear, "outside_schedule"};
    if (lost) return {lowGear, "sensor_lost"};
    if (!fresh || !isfinite(indoor)) return {0, "await_sensor"};

    const float target = targetTenths / 10.0f;
    if (indoor >= target + 0.5f) return {lowGear, "above_target"};
    if (heldMinutes < policy.holdMinutes) return {0, "hold"};

    const float projectedChange = hourlyTrend * policy.forecastMinutes / 60.0f;
    const float projection = indoor + fmaxf(-0.45f, fminf(0.45f, projectedChange));
    const float inputs[] = {target - projection, target - indoor, hourlyTrend, outdoor,
                            lastGear ? lastGear / 10.0f : NAN};
    for (unsigned i = 0; i < policy.count; ++i) {
        const auto& rule = policy.rules[i];
        bool matches = true;
        for (unsigned j = 0; j < rule.count; ++j) {
            const auto& condition = rule.conditions[j];
            const float value = inputs[static_cast<unsigned>(condition.metric)];
            const float threshold = condition.thresholdTenths / 10.0f;
            if (!isfinite(value) || (condition.atLeast ? value < threshold - 0.0001f : value > threshold + 0.0001f)) {
                matches = false;
                break;
            }
        }
        if (matches) return {rule.gear == useLowGear ? lowGear :
                             rule.gear == keepGear ? keepGear :
                             rule.gear > lowGear ? rule.gear : lowGear, "rule", i + 1};
    }
    return {lastGear ? keepGear : lowGear, "near_target"};
}
