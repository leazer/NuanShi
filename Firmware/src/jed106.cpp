#include "jed106.h"

#include <Wire.h>

namespace {
constexpr uint8_t address = 0x2A;
constexpr uint16_t timeoutMs = 25;
constexpr uint8_t e1Command = 0xE1;

bool beginBus() {
    // M5Unified uses controller 0 for the Basic's built-in bus on GPIO21/GPIO22.
    if (!Wire1.begin(jed106::sdaPin, jed106::sclPin, jed106::clockHz)) return false;
    Wire1.setTimeOut(timeoutMs);
    return true;
}

int32_t readCommand(uint8_t command) {
    Wire1.beginTransmission(address);
    Wire1.write(command);
    Wire1.endTransmission(false);
    if (Wire1.requestFrom(address, static_cast<size_t>(2), true) != 2) return -1;
    const int high = Wire1.read();
    const int low = Wire1.read();
    return high * 256 + low;
}

}

jed106::Readings jed106::read() {
    if (!beginBus()) return {-1, -1};
    const auto a1 = readCommand(0xA1);
    const auto e1 = readCommand(e1Command);
    Wire1.end();
    return {a1, e1};
}
