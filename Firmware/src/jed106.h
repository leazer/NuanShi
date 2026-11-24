#pragma once

#include <stdint.h>

namespace jed106 {
constexpr int sdaPin = 26;
constexpr int sclPin = 16;
constexpr uint32_t clockHz = 50000;

struct Readings {
    int32_t a1;
    int32_t e1;
};

Readings read();  // Each value is the unsigned 16-bit response, or -1 on a failed read.
}
