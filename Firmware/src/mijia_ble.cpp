#include "mijia_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <freertos/queue.h>

namespace {
constexpr char serviceId[] = "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr char measurementId[] = "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";
constexpr unsigned retryDelayMs = 10000;
constexpr const char* backupAddresses[] = {"", ""};
std::atomic<int> sampleIntervalMinutes{mijiaDefaultIntervalMinutes};

uint8_t sensorAddress[mijiaAddressSize] = {};
QueueHandle_t readings = nullptr;
QueueHandle_t outcomes = nullptr;
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

void waitForNextSample(TickType_t attemptStarted) {
    for (;;) {
        const TickType_t interval = pdMS_TO_TICKS(mijiaIntervalMs(mijiaIntervalMinutes()));
        const TickType_t elapsed = xTaskGetTickCount() - attemptStarted;
        if (elapsed >= interval) return;
        vTaskDelay(min(interval - elapsed, pdMS_TO_TICKS(1000)));
    }
}

bool readMeasurement(NimBLEClient* client, const NimBLEAddress& target, MijiaSample& sample) {
    ulTaskNotifyTake(pdTRUE, 0);
    xQueueReset(readings);
    bool received = false;
    if (client->connect(target)) {
        NimBLERemoteService* service = client->getService(serviceId);
        NimBLERemoteCharacteristic* measurement = service ? service->getCharacteristic(measurementId) : nullptr;
        if (measurement && measurement->canRead()) {
            const NimBLEAttValue value = measurement->readValue();
            received = publishSample(value.data(), value.size());
        }
        if (!received && measurement && measurement->canNotify() && measurement->subscribe(true, receiveSample)) {
            received = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000)) != 0;
        }
        if (!received) sample.error = measurement ? -3 : service ? -2 : -1;
        if (client->isConnected()) client->disconnect();
    } else {
        sample.error = client->getLastError();
    }
    return received && xQueueReceive(readings, &sample.reading, 0) == pdTRUE;
}

TickType_t readRound(NimBLEClient* client, const NimBLEAddress* targets, const uint8_t* order, unsigned attempts) {
    TickType_t attemptStarted = 0;
    for (unsigned i = 0; i < attempts; ++i) {
        attemptStarted = xTaskGetTickCount();
        MijiaSample sample{};
        sample.source = order[i];
        const bool success = readMeasurement(client, targets[sample.source], sample);
        if (!success) {
            sample.failedAttempts = i + 1;
            sample.exhausted = sample.failedAttempts == attempts;
        }
        xQueueSend(outcomes, &sample, portMAX_DELAY);
        if (success || sample.exhausted) break;
        vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
    }
    return attemptStarted;
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
    const NimBLEAddress targets[] = {
        NimBLEAddress(mac, BLE_ADDR_PUBLIC),
        NimBLEAddress(backupAddresses[0], BLE_ADDR_PUBLIC),
        NimBLEAddress(backupAddresses[1], BLE_ADDR_PUBLIC)
    };
    uint8_t order[mijiaMaxAttempts + 2] = {};
    unsigned attempts = mijiaMaxAttempts;
    for (uint8_t i = 1; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        if (targets[i] != targets[0]) order[attempts++] = i;
    }
    for (;;) {
        waitForNextSample(readRound(client, targets, order, attempts));
    }
}
}  // namespace

int mijiaIntervalMinutes() {
    return sampleIntervalMinutes.load();
}

void setMijiaIntervalMinutes(int minutes) {
    if (validMijiaInterval(minutes)) sampleIntervalMinutes.store(minutes);
}

bool startMijiaReader(const uint8_t (&address)[mijiaAddressSize]) {
    if (readerTask) {
        return false;
    }
    memcpy(sensorAddress, address, sizeof(sensorAddress));
    readings = xQueueCreate(1, sizeof(MijiaReading));
    if (!readings) {
        return false;
    }
    outcomes = xQueueCreate(8, sizeof(MijiaSample));
    if (!outcomes || xTaskCreate(readSensor, "mijia", 6144, nullptr, 1, &readerTask) != pdPASS) {
        if (outcomes) {
            vQueueDelete(outcomes);
            outcomes = nullptr;
        }
        vQueueDelete(readings);
        readings = nullptr;
        return false;
    }
    return true;
}

bool nextMijiaSample(MijiaSample& sample) {
    return outcomes && xQueueReceive(outcomes, &sample, 0) == pdTRUE;
}
