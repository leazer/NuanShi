#include "bench_storage.h"

#include <SD.h>
#include <SPI.h>
#include <math.h>
#include <time.h>

namespace {
constexpr int sdChipSelect = 4;
constexpr char credentialsPath[] = "/nuanshi-wifi.bin";
constexpr char pendingPath[] = "/nuanshi-wifi.tmp";
constexpr char previousPath[] = "/nuanshi-wifi.bak";
constexpr char logPath[] = "/nuanshi.log";
constexpr char temperaturePath[] = "/nuanshi-temperature.bin";
constexpr char temperatureArchivePath[] = "/nuanshi-temperature-archive.bin";
constexpr char temperatureImportPath[] = "/nuanshi-temperature-import.tmp";
constexpr char mijiaPath[] = "/nuanshi-mijia.bin";
constexpr char deviceSettingsPath[] = "/nuanshi-device.json";
constexpr size_t maxDeviceSettingsBytes = 1024;
bool storageMounted = false;

bool readCredentialFile(const char* path, WifiCredentials& credentials) {
    File file = SD.open(path, FILE_READ);
    return file && readCredentials(file, credentials) && file.position() == file.size();
}

bool parseLogTemperature(const char* line, TemperatureRecord& record) {
    constexpr char indoorPrefix[] = " Mijia temperature=";
    constexpr char outdoorPrefix[] = " Outdoor Changping temperature=";
    constexpr size_t timestampLength = 24;
    if (strlen(line) <= timestampLength || strncmp(line + 19, "+0800", 5) != 0) return false;
    const char* event = line + timestampLength;
    const bool outdoor = strncmp(event, outdoorPrefix, sizeof(outdoorPrefix) - 1) == 0;
    if (!outdoor && strncmp(event, indoorPrefix, sizeof(indoorPrefix) - 1) != 0) return false;

    const char* value = event + (outdoor ? sizeof(outdoorPrefix) : sizeof(indoorPrefix)) - 1;
    char* end = nullptr;
    const float temperature = strtof(value, &end);
    if (end == value || strncmp(end, " C", 2) != 0 || !isfinite(temperature) || fabsf(temperature) > 100.0f) {
        return false;
    }
    int16_t targetTenths = TemperatureRecord::outdoor;
    if (!outdoor) {
        const char* target = strstr(end, " target=");
        if (!target) return false;
        target += sizeof(" target=") - 1;
        const float degrees = strtof(target, &end);
        if (end == target || strncmp(end, " C", 2) != 0 || !isfinite(degrees) || degrees < 10 || degrees > 30) {
            return false;
        }
        targetTenths = lroundf(degrees * 10);
    }

    tm local{};
    if (strptime(line, "%Y-%m-%dT%H:%M:%S", &local) != line + 19) return false;
    const time_t epoch = mktime(&local);
    if (epoch <= 0) return false;
    record = {static_cast<uint32_t>(epoch), static_cast<int16_t>(lroundf(temperature * 100)), targetTenths};
    return true;
}

bool readTemperatureTail(File& file, size_t count, TemperatureRecord* records) {
    if (count == 0) return true;
    const size_t completeBytes = file.size() / sizeof(*records) * sizeof(*records);
    const size_t bytes = count * sizeof(*records);
    return file.seek(completeBytes - bytes) && file.read(reinterpret_cast<uint8_t*>(records), bytes) == bytes;
}
}  // namespace

bool mountStorage() {
    SPI.begin(18, 19, 23, sdChipSelect);
    storageMounted = SD.begin(sdChipSelect, SPI, 10000000);
    return storageMounted;
}

void loadDeviceSettings(DeviceSettings& settings) {
    settings = {};
    if (!storageMounted || !SD.exists(deviceSettingsPath)) return;

    File file = SD.open(deviceSettingsPath, FILE_READ);
    const size_t size = file ? file.size() : 0;
    char data[maxDeviceSettingsBytes];
    if (size == 0 || size > sizeof(data) || file.readBytes(data, size) != size ||
        !parseDeviceSettings(data, size, settings)) {
        Serial.println("[SD] nuanshi-device.json invalid or unreadable; using legacy SD settings");
    }
}

bool readCredentials(Stream& source, WifiCredentials& credentials) {
    credentials = {};
    uint8_t sizes[2] = {};
    return source.readBytes(sizes, sizeof(sizes)) == sizeof(sizes) &&
           sizes[0] > 0 && sizes[0] < sizeof(credentials.ssid) &&
           sizes[1] < sizeof(credentials.password) &&
           source.readBytes(credentials.ssid, sizes[0]) == sizes[0] &&
           source.readBytes(credentials.password, sizes[1]) == sizes[1] &&
           strlen(credentials.ssid) == sizes[0] && strlen(credentials.password) == sizes[1];
}

bool loadCredentials(WifiCredentials& credentials) {
    return storageMounted && (readCredentialFile(credentialsPath, credentials) ||
                              readCredentialFile(previousPath, credentials));
}

bool saveCredentials(const WifiCredentials& credentials) {
    if (!storageMounted) {
        return false;
    }
    const uint8_t sizes[] = {static_cast<uint8_t>(strlen(credentials.ssid)),
                             static_cast<uint8_t>(strlen(credentials.password))};
    File pending = SD.open(pendingPath, FILE_WRITE);
    if (!pending) {
        return false;
    }
    const bool written = pending.write(sizes, sizeof(sizes)) == sizeof(sizes) &&
                         pending.write(reinterpret_cast<const uint8_t*>(credentials.ssid), sizes[0]) == sizes[0] &&
                         pending.write(reinterpret_cast<const uint8_t*>(credentials.password), sizes[1]) == sizes[1];
    pending.close();
    WifiCredentials saved;
    if (!written || !readCredentialFile(pendingPath, saved) || strcmp(saved.ssid, credentials.ssid) != 0 ||
        strcmp(saved.password, credentials.password) != 0) {
        return false;
    }
    if (SD.exists(credentialsPath)) {
        if ((SD.exists(previousPath) && !SD.remove(previousPath)) || !SD.rename(credentialsPath, previousPath)) {
            return false;
        }
    }
    if (!SD.rename(pendingPath, credentialsPath)) {
        return false;
    }
    if (SD.exists(previousPath)) {
        SD.remove(previousPath);
    }
    return true;
}

bool loadMijiaAddress(uint8_t (&address)[mijiaAddressSize]) {
    if (!storageMounted || !SD.exists(mijiaPath)) {
        return false;
    }
    File file = SD.open(mijiaPath, FILE_READ);
    return file && file.size() == sizeof(address) && file.read(address, sizeof(address)) == sizeof(address);
}

bool saveMijiaAddress(const uint8_t (&address)[mijiaAddressSize]) {
    if (!storageMounted) {
        return false;
    }
    File file = SD.open(mijiaPath, FILE_WRITE);
    if (!file || file.write(address, sizeof(address)) != sizeof(address)) {
        return false;
    }
    file.close();
    uint8_t saved[mijiaAddressSize];
    return loadMijiaAddress(saved) && memcmp(saved, address, sizeof(address)) == 0;
}

bool appendLog(const char* event, bool clockSynced, bool verify) {
    if (!storageMounted) {
        return false;
    }
    char timestamp[32];
    if (clockSynced) {
        const time_t now = time(nullptr);
        tm local{};
        localtime_r(&now, &local);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local);
    } else {
        snprintf(timestamp, sizeof(timestamp), "uptime_ms=%lu", static_cast<unsigned long>(millis()));
    }

    char line[192];
    const int length = snprintf(line, sizeof(line), "%s %s\n", timestamp, event);
    File file = SD.open(logPath, FILE_APPEND);
    if (!file || length < 1 || static_cast<size_t>(length) >= sizeof(line)) {
        return false;
    }
    const size_t offset = file.size();
    const bool written = file.write(reinterpret_cast<const uint8_t*>(line), length) == static_cast<size_t>(length);
    file.close();
    if (!written || !verify) {
        return written;
    }

    File reopened = SD.open(logPath, FILE_READ);
    char actual[sizeof(line)] = {};
    if (!reopened || reopened.size() != offset + length || !reopened.seek(offset) ||
        reopened.readBytes(actual, length) != static_cast<size_t>(length) || memcmp(actual, line, length) != 0) {
        return false;
    }
    Serial.printf("[SD] log verified bytes=%u entry=%s", static_cast<unsigned>(reopened.size()), actual);
    return true;
}

File openEventLog() {
    return storageMounted ? SD.open(logPath, FILE_READ) : File{};
}

bool deleteEventLog() {
    return storageMounted && (!SD.exists(logPath) || SD.remove(logPath));
}

bool appendTemperatureRecord(const TemperatureRecord& record) {
    if (!storageMounted) {
        return false;
    }
    const bool exists = SD.exists(temperaturePath);
    File file = SD.open(temperaturePath, exists ? "r+" : FILE_APPEND);
    if (!file || (!exists && file.size() != 0)) return false;
    // Overwrite an incomplete final record left by a power loss.
    const size_t alignedEnd = file.size() / sizeof(record) * sizeof(record);
    return file.seek(alignedEnd) &&
           file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
}

int importTemperatureHistory() {
    if (!storageMounted) return -1;
    if (SD.exists(temperatureArchivePath) || !SD.exists(logPath)) return 0;

    uint32_t cutoff = UINT32_MAX;
    if (SD.exists(temperaturePath)) {
        File active = SD.open(temperaturePath, FILE_READ);
        if (!active) return -1;
        TemperatureRecord first;
        if (active.size() >= sizeof(first)) {
            if (active.read(reinterpret_cast<uint8_t*>(&first), sizeof(first)) != sizeof(first)) return -1;
            cutoff = first.epoch;
        }
    }
    File log = openEventLog();
    File pending = SD.open(temperatureImportPath, FILE_WRITE);
    if (!log || !pending) return -1;

    // The system log stores Beijing local time, including before NTP is available at this boot.
    setenv("TZ", "CST-8", 1);
    tzset();
    int count = 0;
    uint32_t firstEpoch = 0;
    uint32_t lastEpoch = 0;
    char line[192];
    for (size_t remaining = log.size(); remaining > 0;) {
        size_t length = 0;
        bool complete = false;
        while (remaining > 0) {
            const int byte = log.read();
            if (byte < 0) return -1;
            --remaining;
            if (byte == '\n') {
                complete = true;
                break;
            }
            if (length < sizeof(line)) line[length++] = byte;
        }
        if (!complete || length == sizeof(line)) continue;

        line[length] = '\0';
        TemperatureRecord record;
        if (!parseLogTemperature(line, record) || record.epoch >= cutoff) continue;
        if (pending.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record)) return -1;
        if (count == 0) firstEpoch = record.epoch;
        lastEpoch = record.epoch;
        ++count;
    }
    pending.close();
    File verify = SD.open(temperatureImportPath, FILE_READ);
    if (!verify || verify.size() != static_cast<size_t>(count) * sizeof(TemperatureRecord)) return -1;
    verify.close();
    if (!SD.rename(temperatureImportPath, temperatureArchivePath)) return -1;
    Serial.printf("[SD] temperature archive records=%d first=%lu last=%lu\n", count,
                  static_cast<unsigned long>(firstEpoch), static_cast<unsigned long>(lastEpoch));
    return count;
}

int readTemperatureHistory(TemperatureRecord* records, size_t capacity) {
    if (!storageMounted) return -1;
    const bool hasActive = SD.exists(temperaturePath);
    File active = hasActive ? SD.open(temperaturePath, FILE_READ) : File{};
    if (hasActive && !active) return -1;
    const size_t activeCount = active ? min(active.size() / sizeof(*records), capacity) : 0;
    size_t archivedCount = 0;
    if (activeCount < capacity && SD.exists(temperatureArchivePath)) {
        File archive = SD.open(temperatureArchivePath, FILE_READ);
        if (!archive) return -1;
        archivedCount = min(archive.size() / sizeof(*records), capacity - activeCount);
        if (!readTemperatureTail(archive, archivedCount, records)) return -1;
    }
    if (!readTemperatureTail(active, activeCount, records + archivedCount)) return -1;
    return static_cast<int>(archivedCount + activeCount);
}
