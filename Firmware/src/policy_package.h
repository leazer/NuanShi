#pragma once

#include <Arduino.h>
#include "heating_policy.h"

bool loadBuiltInPolicy(HeatingPolicy& policy);
bool decodePolicy(const String& package, HeatingPolicy& policy);
String encodePolicy(const HeatingPolicy& policy);
