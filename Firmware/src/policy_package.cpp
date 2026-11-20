#include "policy_package.h"

#include <ArduinoJson.h>

extern const char defaultPolicyStart[] asm("_binary_strategies_nuanshi_v1_json_start");
extern const char defaultPolicyEnd[] asm("_binary_strategies_nuanshi_v1_json_end");

namespace {
constexpr const char* metricNames[] = {"forecast_deficit", "indoor_deficit", "trend", "outdoor", "gear"};

bool decode(const char* data, size_t size, HeatingPolicy& policy) {
    if (size == 0 || size > 4096) return false;
    JsonDocument doc;
    if (deserializeJson(doc, data, size)) return false;
    const auto root = doc.as<JsonObjectConst>();
    if (root.size() != 5 || !root["schema"].is<int>() || root["schema"].as<int>() != 1 ||
        !root["revision"].is<uint16_t>() || !root["hold_minutes"].is<uint16_t>() ||
        !root["forecast_minutes"].is<uint16_t>()) return false;
    const auto rules = root["rules"].as<JsonArrayConst>();
    if (!rules.size() || rules.size() > maxHeatingRules) return false;

    HeatingPolicy draft{};
    draft.revision = root["revision"].as<uint16_t>();
    draft.holdMinutes = root["hold_minutes"].as<uint16_t>();
    draft.forecastMinutes = root["forecast_minutes"].as<uint16_t>();
    for (JsonObjectConst entry : rules) {
        if (entry.size() != 2) return false;
        auto& rule = draft.rules[draft.count++];
        const auto gear = entry["gear"];
        if (gear.is<int16_t>() && gear.as<int16_t>() >= 5 && gear.as<int16_t>() <= 40) {
            rule.gear = gear.as<int16_t>();
        } else if (gear.is<const char*>() && strcmp(gear.as<const char*>(), "low") == 0) {
            rule.gear = useLowGear;
        } else if (gear.is<const char*>() && strcmp(gear.as<const char*>(), "hold") == 0) {
            rule.gear = keepGear;
        } else {
            return false;
        }

        const auto conditions = entry["all"].as<JsonArrayConst>();
        if (!conditions.size() || conditions.size() > maxHeatingConditions) return false;
        for (JsonObjectConst match : conditions) {
            if (match.size() != 3 || !match["tenths"].is<int16_t>()) return false;
            const char* name = match["metric"] | "";
            unsigned index = 0;
            while (index < sizeof(metricNames) / sizeof(metricNames[0]) && strcmp(name, metricNames[index]) != 0) ++index;
            if (index == sizeof(metricNames) / sizeof(metricNames[0])) return false;
            const char* op = match["op"] | "";
            const bool atLeast = strcmp(op, ">=") == 0;
            if (!atLeast && strcmp(op, "<=") != 0) return false;
            rule.conditions[rule.count++] = {
                static_cast<HeatingMetric>(index), atLeast, match["tenths"].as<int16_t>()
            };
        }
    }
    if (!validHeatingPolicy(draft)) return false;
    policy = draft;
    return true;
}
}  // namespace

bool loadBuiltInPolicy(HeatingPolicy& policy) {
    return decode(defaultPolicyStart, defaultPolicyEnd - defaultPolicyStart, policy);
}

bool decodePolicy(const String& package, HeatingPolicy& policy) {
    return decode(package.c_str(), package.length(), policy);
}

String encodePolicy(const HeatingPolicy& policy) {
    JsonDocument doc;
    doc["schema"] = 1;
    doc["revision"] = policy.revision;
    doc["hold_minutes"] = policy.holdMinutes;
    doc["forecast_minutes"] = policy.forecastMinutes;
    auto rules = doc["rules"].to<JsonArray>();
    for (unsigned i = 0; i < policy.count; ++i) {
        const auto& rule = policy.rules[i];
        auto entry = rules.add<JsonObject>();
        if (rule.gear == useLowGear) entry["gear"] = "low";
        else if (rule.gear == keepGear) entry["gear"] = "hold";
        else entry["gear"] = rule.gear;
        auto conditions = entry["all"].to<JsonArray>();
        for (unsigned j = 0; j < rule.count; ++j) {
            const auto& condition = rule.conditions[j];
            auto match = conditions.add<JsonObject>();
            match["metric"] = metricNames[static_cast<unsigned>(condition.metric)];
            match["op"] = condition.atLeast ? ">=" : "<=";
            match["tenths"] = condition.thresholdTenths;
        }
    }
    String result;
    serializeJsonPretty(doc, result);
    return result;
}
