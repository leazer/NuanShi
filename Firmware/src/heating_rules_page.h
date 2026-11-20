#pragma once

#include "heating_policy.h"

constexpr unsigned heatingRulesPerPage = 2;

inline unsigned heatingRulesPageCount(const HeatingPolicy& policy) {
    if (!validHeatingPolicy(policy)) return 1;
    return (policy.count + heatingRulesPerPage - 1) / heatingRulesPerPage + 1;
}

void drawHeatingRulesPage(const HeatingPolicy& policy, unsigned selectedPage, unsigned start, unsigned end, int lowGear);
