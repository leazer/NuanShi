#include "mijia_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/queue.h>

namespace {
constexpr char serviceId[] = "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char measurementId[] = "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr unsigned sampleIntervalMs = 60000;

uint8_t sensorAddress[mijiaAddressSize] = {};
QueueHandle_t readings = nullptr;
TaskHandle_t readerTask = nullptr;

bool publishSample(const uint8_t* data, size_t size) {
    if (size != 5 || data[2] > 100) {
        return false;
    }
    const uint16_t raw = data[0] | (static_cast<uint16_t>(data[1]) << 8);
    const MijiaReading reading{
        static_cast<int16_t>(raw) / 100.0f,
        data[2],
        static_cast<unsigned>(data[3] | (static_cast<uint16_t>(data[4]) << 8)),
    };
    xQueueOverwrite(readings, &reading);
    return true;
}

void receiveSample(NimBLERemoteCharacteristic*, uint8_t* data, size_t size, bool) {
    if (publishSample(data, size)) {
        xTaskNotifyGive(readerTask);
    }
}

void readSensor(void*) {
    NimBLEClient* client = NimBLEDevice::createClient();
    if (!client) {
        Serial.println("[MIJIA] BLE client unavailable");
        vTaskDelete(nullptr);
        return;
    }
    client->setConnectTimeout(10000);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", sensorAddress[0], sensorAddress[1],
             sensorAddress[2], sensorAddress[3], sensorAddress[4], sensorAddress[5]);
    const NimBLEAddress target(mac, BLE_ADDR_PUBLIC);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, 0);
        if (client->connect(target)) {
            NimBLERemoteService* service = client->getService(serviceId);
            NimBLERemoteCharacteristic* measurement = service ? service->getCharacteristic(measurementId) : nullptr;
            bool received = false;
            if (measurement && measurement->canRead()) {
                const NimBLEAttValue value = measurement->readValue();
                received = publishSample(value.data(), value.size());
            }
            if (!received && measurement && measurement->canNotify() && measurement->subscribe(true, receiveSample)) {
                received = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000)) != 0;
            }
            if (!received) {
                Serial.printf("[MIJIA] no measurement (service=%d characteristic=%d)\n", service != nullptr,
                              measurement != nullptr);
            }
            if (client->isConnected()) {
                client->disconnect();
            }
        } else {
            Serial.printf("[MIJIA] BLE connection failed code=%d\n", client->getLastError());
        }
        vTaskDelay(pdMS_TO_TICKS(sampleIntervalMs));
    }
}
}  // namespace

bool startMijiaReader(const uint8_t (&address)[mijiaAddressSize]) {
    if (readerTask) {
        return false;
    }
    memcpy(sensorAddress, address, sizeof(sensorAddress));
    readings = xQueueCreate(1, sizeof(MijiaReading));
    if (!readings) {
        return false;
    }
    if (xTaskCreate(readSensor, "mijia", 6144, nullptr, 1, &readerTask) != pdPASS) {
        vQueueDelete(readings);
        readings = nullptr;
        return false;
    }
    return true;
}

bool nextMijiaReading(MijiaReading& reading) {
    return readings && xQueueReceive(readings, &reading, 0) == pdTRUE;
}
