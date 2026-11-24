#include "heating_rules_page.h"

#include <M5Unified.h>

namespace {
constexpr uint16_t cardColor = 0x10E5;
constexpr uint16_t mutedColor = 0xA5BA;
constexpr uint16_t gearColor = 0xFE2F;

void text(const char* value, int x, int y, uint16_t color = TFT_WHITE) {
    auto& display = M5.Display;
    display.setTextColor(color);
    display.setCursor(x, y);
    display.print(value);
}

void drawRule(const HeatingRule& rule, unsigned index, int y, int lowGear) {
    auto& display = M5.Display;
    display.fillRoundRect(12, y, 296, 72, 6, cardColor);
    char label[96];
    display.setFont(&fonts::efontCN_16);
    snprintf(label, sizeof(label), "规则 %u", index + 1);
    text(label, 23, y + 6, TFT_CYAN);

    if (rule.gear == keepGear) {
        snprintf(label, sizeof(label), "维持原档");
    } else if (rule.gear == useLowGear) {
        snprintf(label, sizeof(label), "低温档 %.1f", lowGear / 10.0f);
    } else {
        snprintf(label, sizeof(label), "%.1f 档", (rule.gear < lowGear ? lowGear : rule.gear) / 10.0f);
    }
    text(label, 297 - display.textWidth(label), y + 6, gearColor);

    constexpr const char* names[] = {"预计欠温", "实测欠温", "室温趋势", "室外温度", "当前档位"};
    constexpr const char* units[] = {"°C", "°C", "°C/h", "°C", "档"};
    display.setFont(&fonts::efontCN_12);
    for (unsigned i = 0; i < rule.count; ++i) {
        const auto& condition = rule.conditions[i];
        const auto metric = static_cast<unsigned>(condition.metric);
        snprintf(label, sizeof(label), "%s  %s  %.1f %s", names[metric],
                 condition.atLeast ? ">=" : "<=", condition.thresholdTenths / 10.0f, units[metric]);
        text(label, 23, y + 26 + i * 14);
    }
}

void drawLimits(const HeatingPolicy& policy, unsigned start, unsigned end, int lowGear) {
    auto& display = M5.Display;
    display.fillRoundRect(12, 53, 296, 153, 6, cardColor);
    display.setFont(&fonts::efontCN_16);
    text("时段与保护", 23, 60, TFT_CYAN);
    display.setFont(&fonts::efontCN_12);
    char line[96];
    if (start == end) {
        snprintf(line, sizeof(line), "全天低温档 %.1f", lowGear / 10.0f);
    } else {
        snprintf(line, sizeof(line), "采暖 %02u:%02u-%02u:%02u   低温档 %.1f",
                 start / 60, start % 60, end / 60, end % 60, lowGear / 10.0f);
    }
    text(line, 23, 86);
    snprintf(line, sizeof(line), "常规间隔 %u 分钟   预测 %u 分钟", policy.holdMinutes, policy.forecastMinutes);
    text(line, 23, 105);
    text("短时失联保持，持续失联回低温档", 23, 124);
    text("恢复稳定读数后立即重新判断", 23, 143);
    text("非采暖时段或超温 0.5 °C：回低温档", 23, 162);
    text("读数突变：等待稳定后再判断", 23, 181);
}
}  // namespace

void drawHeatingRulesPage(const HeatingPolicy& policy, unsigned selectedPage, unsigned start, unsigned end, int lowGear) {
    auto& display = M5.Display;
    display.setFont(&fonts::efontCN_12);
    if (!validHeatingPolicy(policy)) {
        text("暂无有效策略", 118, 102, mutedColor);
        return;
    }

    const unsigned pageCount = heatingRulesPageCount(policy);
    const bool limitsPage = selectedPage == pageCount - 1;
    char summary[64];
    snprintf(summary, sizeof(summary), "v%u   %s", policy.revision,
             limitsPage ? "固件保护" : "顺序匹配 · 条件需同时满足");
    text(summary, 13, 35, mutedColor);
    snprintf(summary, sizeof(summary), "%u/%u", selectedPage + 1, pageCount);
    text(summary, 306 - display.textWidth(summary), 35, mutedColor);

    if (limitsPage) {
        drawLimits(policy, start, end, lowGear);
        return;
    }
    const unsigned first = selectedPage * heatingRulesPerPage;
    for (unsigned i = 0; i < heatingRulesPerPage && first + i < policy.count; ++i) {
        drawRule(policy.rules[first + i], first + i, 53 + i * 79, lowGear);
    }
}
