#include "bench_storage.h"

#include <SD.h>
#include <SPI.h>
#include <time.h>

namespace {
constexpr int sdChipSelect = 4;
constexpr char credentialsPath[] = "/nuanshi-wifi.bin";
constexpr char pendingPath[] = "/nuanshi-wifi.tmp";
constexpr char previousPath[] = "/nuanshi-wifi.bak";
constexpr char logPath[] = "/nuanshi.log";
constexpr char mijiaPath[] = "/nuanshi-mijia.bin";
bool storageMounted = false;

bool readCredentialFile(const char* path, WifiCredentials& credentials) {
    File file = SD.open(path, FILE_READ);
    return file && readCredentials(file, credentials) && file.position() == file.size();
}
}  // namespace

bool mountStorage() {
    SPI.begin(18, 19, 23, sdChipSelect);
    storageMounted = SD.begin(sdChipSelect, SPI, 10000000);
    return storageMounted;
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
