#include "changping_weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/queue.h>

#include "bench_storage.h"
#include "weather_roots.h"

namespace {
constexpr char endpoint[] = "https://api.open-meteo.com/v1/forecast?latitude=40.22&longitude=116.23"
                            "&current=temperature_2m&timeformat=unixtime&forecast_days=1";
QueueHandle_t outdoorSamples = nullptr;
constexpr uint32_t refreshMs = 30 * 60 * 1000UL;
constexpr uint32_t retryMs = 60 * 1000UL;
constexpr size_t cacheLogTailBytes = 24 * 1024;

OutdoorSample readOutdoor() {
    WiFiClientSecure connection;
    connection.setCACert(weatherRootCertificates);
    connection.setHandshakeTimeout(8);
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    if (!http.begin(connection, endpoint)) {
        return {NAN, 0, -1};
    }
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        if (status < 0) {
            char detail[128]{};
            const int tlsError = connection.lastError(detail, sizeof(detail));
            Serial.printf("[WEATHER] HTTPS code=%d TLS=%d %s\n", status, tlsError, detail);
        }
        return {NAN, 0, status};
    }
    JsonDocument result;
    if (deserializeJson(result, http.getStream(), DeserializationOption::NestingLimit(5))) {
        return {NAN, 0, -2};
    }
    const float temperature = result["current"]["temperature_2m"] | NAN;
    const time_t observedAt = result["current"]["time"].as<time_t>();
    const OutdoorSample sample{temperature, observedAt, 0};
    if (!isFreshOutdoorSample(sample, time(nullptr))) {
        return {NAN, 0, -3};
    }
    return sample;
}

bool parseCachedOutdoor(const char* line, time_t now, OutdoorSample& sample) {
    float temperature;
    long long observedAt;
    if (strlen(line) <= 24 || strncmp(line + 19, "+0800", 5) != 0 ||
        sscanf(line + 24, " Outdoor Changping temperature=%f C source_epoch=%lld", &temperature, &observedAt) != 2 ||
        observedAt <= 0 || observedAt > UINT32_MAX) return false;

    const OutdoorSample candidate{temperature, static_cast<time_t>(observedAt), 0};
    if (!isFreshOutdoorSample(candidate, now)) return false;
    sample = candidate;
    return true;
}

void outdoorLoop(void*) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        const OutdoorSample sample = readOutdoor();
        xQueueOverwrite(outdoorSamples, &sample);
        vTaskDelay(pdMS_TO_TICKS(sample.error == 0 ? refreshMs : retryMs));
    }
}
}  // namespace

bool isFreshOutdoorSample(const OutdoorSample& sample, time_t now) {
    const double age = difftime(now, sample.observedAt);
    return sample.error == 0 && sample.observedAt > 0 && isfinite(sample.temperature) &&
           sample.temperature >= -60 && sample.temperature <= 65 && age >= -900 && age <= 5400;
}

bool loadRecentOutdoorSample(OutdoorSample& sample) {
    File log = openEventLog();
    if (!log) return false;
    const size_t size = log.size();
    const size_t offset = size > cacheLogTailBytes ? size - cacheLogTailBytes : 0;
    if (!log.seek(offset)) return false;

    const time_t now = time(nullptr);
    OutdoorSample newest{};
    char line[128];
    size_t length = 0;
    bool partial = offset != 0;
    for (size_t i = offset; i < size; ++i) {
        const int byte = log.read();
        if (byte < 0) break;
        if (byte == '\n') {
            if (!partial) {
                line[length] = '\0';
                OutdoorSample candidate{};
                if (parseCachedOutdoor(line, now, candidate) && candidate.observedAt >= newest.observedAt) {
                    newest = candidate;
                }
            }
            length = 0;
            partial = false;
        } else if (!partial) {
            if (length < sizeof(line) - 1) line[length++] = static_cast<char>(byte);
            else partial = true;
        }
    }
    if (newest.observedAt == 0) return false;
    sample = newest;
    return true;
}

bool startOutdoorReader() {
    outdoorSamples = xQueueCreate(1, sizeof(OutdoorSample));
    if (!outdoorSamples) {
        return false;
    }
    if (xTaskCreate(outdoorLoop, "weather", 8192, nullptr, 1, nullptr) != pdPASS) {
        vQueueDelete(outdoorSamples);
        outdoorSamples = nullptr;
        return false;
    }
    return true;
}

bool nextOutdoorSample(OutdoorSample& sample) {
    return outdoorSamples && xQueueReceive(outdoorSamples, &sample, 0) == pdTRUE;
}
