#include "changping_weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/queue.h>

#include "weather_roots.h"

namespace {
constexpr char endpoint[] = "https://api.open-meteo.com/v1/forecast?latitude=40.22&longitude=116.23"
                            "&current=temperature_2m&timeformat=unixtime&forecast_days=1";
QueueHandle_t outdoorSamples = nullptr;

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
        return {NAN, 0, status};
    }
    JsonDocument result;
    if (deserializeJson(result, http.getStream(), DeserializationOption::NestingLimit(5))) {
        return {NAN, 0, -2};
    }
    const float temperature = result["current"]["temperature_2m"] | NAN;
    const time_t observedAt = result["current"]["time"].as<time_t>();
    if (!isfinite(temperature) || temperature < -60 || temperature > 65 || fabs(difftime(time(nullptr), observedAt)) > 5400) {
        return {NAN, 0, -3};
    }
    return {temperature, observedAt, 0};
}

void outdoorLoop(void*) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        const OutdoorSample sample = readOutdoor();
        xQueueOverwrite(outdoorSamples, &sample);
        vTaskDelay(pdMS_TO_TICKS(sample.error == 0 ? 1800000 : 300000));
    }
}
}  // namespace

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
