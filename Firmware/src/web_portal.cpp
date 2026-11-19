#include "web_portal.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include "bench_storage.h"
#include "temperature_chart.h"
#include "web_history.h"

namespace {
constexpr uint32_t sessionLifetimeMs = 3600000;
constexpr uint32_t loginRetryMs = 1000;
constexpr size_t logPreviewBytes = 8 * 1024;
constexpr char passwordHash[] = "0000000000000000000000000000000000000000000000000000000000000000";
WebServer server(80);
String session;
String formToken;
uint32_t sessionStarted = 0;
uint32_t loginFailedAt = 0;
bool loginFailed = false;
PortalStatus (*readStatus)();
bool (*saveTargetSetting)(int);
bool (*saveHeatingSchedule)(uint16_t, uint16_t);
bool (*saveLowHeatingGear)(int);
bool (*saveModeSetting)(bool);
bool (*applyManualGear)(int);

constexpr char pageStart[] = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>暖时</title>
<style>
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;color:#e6eff5;background:#101923;margin:0}
main{max-width:720px;padding:24px 16px 48px;margin:auto}h1{margin:0 0 16px}h2{font-size:1.15rem;margin:0 0 14px}
section{background:#192734;border:1px solid #2c4150;border-radius:12px;padding:18px;margin-bottom:14px}
dl{display:grid;grid-template-columns:auto 1fr;gap:8px 20px;margin:0}dd{margin:0;overflow-wrap:anywhere}
dt,small{color:#b9c9d6}form{margin:0}form+form{border-top:1px solid #2c4150;padding-top:12px;margin-top:12px}
label{display:inline-flex;align-items:center;gap:8px;margin:0 8px 8px 0}
input,select,button,a.button{font:inherit;padding:8px 10px;border-radius:6px;border:1px solid #526a7b;color:inherit;background:#111d28}
input[type=number]{width:5.5rem}button,a.button{cursor:pointer;text-decoration:none;display:inline-block}
button:hover,a.button:hover{border-color:#7ed8e6}button.danger,.danger{color:#ff9c9a}.warn{color:#ffd18f}.good{color:#8fddd1}
fieldset{padding:0;border:0;margin:14px 0 0;min-width:0}fieldset:disabled,button:disabled{opacity:.55;cursor:default}
.actions{display:flex;gap:12px;flex-wrap:wrap}.actions form{display:inline}
#temperature-chart{display:block;width:100%;height:300px;background:#111d28;border-radius:6px;touch-action:pan-y}
.chart-key{display:flex;gap:8px 14px;flex-wrap:wrap;margin:0 0 10px;font-size:.83rem}
#chart-note{display:block;min-height:1.25rem;margin-top:8px}
#log-preview{background:#111d28;border:1px solid #2c4150;border-radius:6px;padding:12px;margin:14px 0 8px;
max-height:20rem;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere;font:0.83rem/1.55 ui-monospace,monospace}
</style><main><h1>暖时</h1>)HTML";

void responseHeaders() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    server.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; "
                      "script-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}

void sendText(int code, const char* message) {
    responseHeaders();
    server.send(code, "text/plain; charset=utf-8", message);
}

void redirectHome() {
    responseHeaders();
    server.sendHeader("Location", "/");
    server.send(303);
}

String randomToken() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    char hex[sizeof(bytes) * 2 + 1];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned>(bytes[i]));
    }
    return String(hex);
}

String cookieSession() {
    const String cookies = server.header("Cookie");
    unsigned start = 0;
    while (start < cookies.length()) {
        const int separator = cookies.indexOf(';', start);
        const unsigned end = separator < 0 ? cookies.length() : static_cast<unsigned>(separator);
        String entry = cookies.substring(start, end);
        entry.trim();
        if (entry.startsWith("nuanshi=")) {
            return entry.substring(8);
        }
        start = end + 1;
    }
    return {};
}

bool loggedIn() {
    return session.length() == 32 && millis() - sessionStarted < sessionLifetimeMs &&
           session.equalsConstantTime(cookieSession());
}

bool requireLogin() {
    if (loggedIn()) return true;
    sendText(401, "请先打开设备首页登录。");
    return false;
}

bool requireFormToken() {
    if (!requireLogin()) return false;
    const String supplied = server.arg("token");
    if (formToken.length() == 32 && formToken.equalsConstantTime(supplied)) return true;
    sendText(403, "请求校验失败，请刷新首页后重试。");
    return false;
}

bool passwordCorrect(const String& password) {
    if (password.length() == 0 || password.length() > 64) return false;
    uint8_t hash[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(password.c_str()), password.length(), hash, 0) != 0) {
        return false;
    }
    unsigned difference = 0;
    for (size_t i = 0; i < sizeof(hash); ++i) {
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        difference |= hash[i] ^ (digit(passwordHash[2 * i]) * 16 + digit(passwordHash[2 * i + 1]));
    }
    return difference == 0;
}

void sendLogin(int code = 200, const char* error = "") {
    String html(pageStart);
    html += F("<section><h2>设备登录</h2><form method=post action=/login>"
              "<label>密码 <input name=password type=password autocomplete=current-password required maxlength=64 autofocus></label>"
              "<button>登录</button></form><p class=warn>");
    html += error;
    html += F("</p></section></main></html>");
    responseHeaders();
    server.send(code, "text/html; charset=utf-8", html);
}

void login() {
    if (loginFailed && millis() - loginFailedAt < loginRetryMs) {
        sendLogin(429, "请稍后再试。");
        return;
    }
    if (!passwordCorrect(server.arg("password"))) {
        loginFailed = true;
        loginFailedAt = millis();
        sendLogin(401, "密码错误。");
        return;
    }
    loginFailed = false;
    session = randomToken();
    formToken = randomToken();
    sessionStarted = millis();
    server.sendHeader("Set-Cookie", "nuanshi=" + session + "; Path=/; Max-Age=3600; HttpOnly; SameSite=Strict");
    redirectHome();
}

String temperature(float degrees) {
    return isfinite(degrees) ? String(degrees, 1) + " °C" : String("暂无读数");
}

String statusHtml(const PortalStatus& data) {
    String html;
    html.reserve(720);
    html += data.manual ? F("<dl data-mode=manual>") : F("<dl data-mode=auto>");
    html += F("<dt>控制模式</dt><dd>");
    html += data.manual ? "手动控制" : "自动控制 · 观测中";
    html += F("</dd><dt>最后档位指令</dt><dd>");
    html += data.gearTenths ? String(data.gearTenths / 10.0f, 1) + " 档" : String("暂无记录");
    html += F("</dd><dt>室内温度</dt><dd class=");
    html += data.roomTone;
    html += '>';
    html += temperature(data.indoor);
    html += F(" · ");
    html += data.roomState;
    if (isfinite(data.indoor)) {
        html += F(" · ");
        html += data.indoorAgeMinutes == 0 ? String("刚刚更新") : String(data.indoorAgeMinutes) + " 分钟前";
    }
    html += F("</dd><dt>室外温度</dt><dd>");
    html += temperature(data.outdoor);
    html += F("</dd><dt>目标温度</dt><dd>");
    html += temperature(data.targetTenths / 10.0f);
    html += F("</dd><dt>WiFi</dt><dd>");
    html += WiFi.localIP().toString();
    html += F(" · ");
    html += String(WiFi.RSSI());
    html += F(" dBm</dd><dt>SD 卡</dt><dd>");
    html += data.sdMounted ? "已挂载" : "未挂载";
    html += F("</dd></dl>");
    return html;
}

void addForm(String& html, const char* action) {
    html += F("<form method=post action=");
    html += action;
    html += F("><input type=hidden name=token value=\"");
    html += formToken;
    html += F("\">");
}

void dashboard() {
    if (!loggedIn()) {
        sendLogin();
        return;
    }
    const PortalStatus data = readStatus();
    String html;
    html.reserve(14000);
    html += pageStart;
    html += F("<section><h2>当前状态</h2><div id=status>");
    html += statusHtml(data);
    html += F("</div></section><section><h2>设置</h2>");
    addForm(html, "/settings/mode");
    html += F("<label>控制模式 <select name=value><option value=auto");
    if (!data.manual) html += F(" selected");
    html += F(">自动控制</option><option value=manual");
    if (data.manual) html += F(" selected");
    html += F(">手动控制</option></select></label><button>保存</button></form>");
    if (data.manual) {
        addForm(html, "/control/gear");
        html += data.calibrated ? F("<div class=actions>") : F("<fieldset disabled><div class=actions>");
        html += F("<button name=value value=1>1 档</button><button name=value value=2>2 档</button>"
                  "<button name=value value=3>3 档</button></div>");
        if (!data.calibrated) html += F("</fieldset><small>请先在设备上完成舵机标定。</small>");
        html += F("</form>");
    }
    html += data.manual ? F("<fieldset disabled>") : F("<fieldset>");
    addForm(html, "/settings/target");
    html += F("<label>目标温度 <input type=number name=value min=10 max=30 step=0.5 required value=");
    html += String(data.targetTenths / 10.0f, 1);
    html += F("> °C</label><button>保存</button></form>");
    addForm(html, "/settings/schedule");
    char times[200];
    snprintf(times, sizeof(times), "<label>起始时间 <input type=time name=start required value=%02u:%02u></label>"
             "<label>截止时间 <input type=time name=end required value=%02u:%02u></label>",
             static_cast<unsigned>(data.heatingStart / 60), static_cast<unsigned>(data.heatingStart % 60),
             static_cast<unsigned>(data.heatingEnd / 60), static_cast<unsigned>(data.heatingEnd % 60));
    html += times;
    html += F("<button>保存</button></form>");
    addForm(html, "/settings/low-gear");
    html += F("<label>低温加热档位 <input type=number name=value min=0.5 max=3 step=0.1 required value=");
    html += String(data.lowGearTenths / 10.0f, 1);
    html += F("></label><button>保存</button></form></fieldset></section>"
              "<section><h2>温度曲线</h2><div class=chart-key>"
              "<span style=color:#79dfc4>室内温度 °C</span><span style=color:#93b6ff>室外温度 °C</span>"
              "<span style=color:#ffc885>手动档位</span><span style=color:#c7a8f1>测试档位</span></div>"
              "<canvas id=temperature-chart aria-label=\"最近 24 小时的室温、室外温度和档位指令\"></canvas>"
              "<small id=chart-note>正在读取温度曲线…</small></section>"
              "<section><h2>系统日志</h2><div class=actions><a class=button href=/nuanshi.log>下载日志</a>");
    addForm(html, "/logs/delete");
    html += F("<button class=danger onclick=\"return confirm('删除当前系统日志？此操作不可撤销。')\">删除日志</button>"
              "</form></div><pre id=log-preview tabindex=0 aria-label=\"最新系统日志\">正在读取日志…</pre>"
              "<small>预览文件末尾，完整记录可下载。</small></section><script>"
              "const preview=document.getElementById('log-preview');"
              "async function updateLog(){if(document.hidden)return;try{"
              "const r=await fetch('/logs/preview');if(r.status===401){location.reload();return;}"
              "const text=await r.text()||'暂无系统日志。';if(text===preview.textContent)return;"
              "const atEnd=preview.scrollHeight-preview.clientHeight-preview.scrollTop<4;"
              "preview.textContent=text;if(atEnd)preview.scrollTop=preview.scrollHeight;"
              "}catch{preview.textContent='日志读取失败，稍后自动重试。'}}"
              "updateLog();setInterval(updateLog,30000);"
              "document.addEventListener('visibilitychange',updateLog);"
              "setInterval(async()=>{try{const r=await fetch('/status');"
              "if(r.ok){const s=document.getElementById('status'),mode=s.firstElementChild.dataset.mode;"
              "s.innerHTML=await r.text();if(s.firstElementChild.dataset.mode!==mode)location.reload();}"
              "else if(r.status===401)location.reload()}catch{}},10000)"
              "</script>");
    html += webHistoryScript;
    html += F("</main></html>");
    responseHeaders();
    server.send(200, "text/html; charset=utf-8", html);
}

// Accept a short unsigned decimal with up to one fractional digit.
int parseTenths(const String& value) {
    const size_t length = value.length();
    if (length == 0 || length > 4) return -1;
    int whole = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = value[i];
        if (i > 0 && c == '.' && i + 2 == length && isDigit(value[i + 1])) {
            return whole * 10 + value[i + 1] - '0';
        }
        if (!isDigit(c)) return -1;
        whole = whole * 10 + c - '0';
    }
    return whole * 10;
}

int parseTime(const String& value) {
    if (value.length() != 5 || value[2] != ':' ||
        !isDigit(value[0]) || !isDigit(value[1]) || !isDigit(value[3]) || !isDigit(value[4])) return -1;
    const int hours = (value[0] - '0') * 10 + value[1] - '0';
    const int minutes = (value[3] - '0') * 10 + value[4] - '0';
    return hours < 24 && minutes < 60 ? hours * 60 + minutes : -1;
}

void saveResult(bool success) {
    if (success) redirectHome();
    else sendText(500, "保存设置失败。");
}

bool requireAutomaticMode() {
    if (!requireFormToken()) return false;
    if (!readStatus().manual) return true;
    sendText(409, "手动控制期间暂停此设置。");
    return false;
}

void sendHistory() {
    if (!requireLogin()) return;
    const PortalStatus status = readStatus();
    if (!status.sdMounted || status.epoch <= temperatureHistoryWindow) {
        sendText(503, status.sdMounted ? "等待校准时间。" : "SD 卡未挂载。");
        return;
    }
    const TemperatureRecord* records;
    const int count = temperatureChartRecords(records);
    if (count < 0) {
        sendText(503, "温度曲线暂时无法读取。");
        return;
    }
    const uint32_t start = status.epoch - temperatureHistoryWindow;
    responseHeaders();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv; charset=utf-8", "");
    char chunk[768];
    size_t used = snprintf(chunk, sizeof(chunk), "%lu,%lu\n",
                           static_cast<unsigned long>(start), static_cast<unsigned long>(status.epoch));
    for (int i = 0; i < count; ++i) {
        const auto& row = records[i];
        if (row.epoch < start || row.epoch > status.epoch) continue;
        const bool gear = row.targetTenths == TemperatureRecord::gear || row.targetTenths == TemperatureRecord::testGear;
        const char* type = row.targetTenths == TemperatureRecord::outdoor ? "outdoor" :
                           row.targetTenths == TemperatureRecord::gear ? "gear" :
                           row.targetTenths == TemperatureRecord::testGear ? "test" : "indoor";
        char line[48];
        const int size = snprintf(line, sizeof(line), "%lu,%s,%.*f\n", static_cast<unsigned long>(row.epoch),
                                  type, gear ? 1 : 2, row.temperatureHundredths / (gear ? 10.0 : 100.0));
        if (size <= 0 || static_cast<size_t>(size) >= sizeof(line)) continue;
        if (used + size > sizeof(chunk)) {
            server.sendContent(chunk, used);
            used = 0;
        }
        memcpy(chunk + used, line, size);
        used += size;
    }
    server.sendContent(chunk, used);
    if (used) server.sendContent("", 0);
}

bool readLogTail(File& log, String& preview) {
    const size_t fileSize = log.size();
    size_t remaining = min(fileSize, logPreviewBytes);
    const size_t start = fileSize - remaining;
    bool skipPartialLine = false;
    if (start > 0) {
        if (!log.seek(start - 1)) return false;
        const int before = log.read();
        if (before < 0) return false;
        skipPartialLine = before != '\n';
    }
    if (!preview.reserve(remaining)) return false;
    char block[257];
    while (remaining > 0) {
        const size_t count = log.readBytes(block, min(remaining, sizeof(block) - 1));
        if (count == 0) return false;
        block[count] = '\0';
        remaining -= count;
        const char* data = block;
        if (skipPartialLine) {
            const auto* newline = static_cast<const char*>(memchr(block, '\n', count));
            if (!newline) continue;
            data = newline + 1;
            skipPartialLine = false;
        }
        if (data != block + count && !preview.concat(data, block + count - data)) return false;
    }
    return true;
}

void sendLogPreview() {
    if (!requireLogin()) return;
    if (!readStatus().sdMounted) {
        sendText(503, "SD 卡未挂载。");
        return;
    }
    File log = openEventLog();
    if (!log) {
        sendText(404, "暂无系统日志。");
        return;
    }
    String preview;
    if (!readLogTail(log, preview)) {
        sendText(503, "系统日志暂时无法读取。");
        return;
    }
    responseHeaders();
    server.send(200, "text/plain; charset=utf-8", preview);
}
}  // namespace

void startWebPortal(PortalStatus (*status)(), bool (*target)(int),
                    bool (*schedule)(uint16_t, uint16_t), bool (*lowGear)(int),
                    bool (*mode)(bool), bool (*manualGear)(int)) {
    readStatus = status;
    saveTargetSetting = target;
    saveHeatingSchedule = schedule;
    saveLowHeatingGear = lowGear;
    saveModeSetting = mode;
    applyManualGear = manualGear;
    const char* headers[] = {"Cookie"};
    server.collectHeaders(headers, 1);
    server.on("/", HTTP_GET, dashboard);
    server.on("/login", HTTP_POST, login);
    server.on("/status", HTTP_GET, []() {
        if (!requireLogin()) return;
        responseHeaders();
        server.send(200, "text/html; charset=utf-8", statusHtml(readStatus()));
    });
    server.on("/settings/target", HTTP_POST, []() {
        if (!requireAutomaticMode()) return;
        const int value = parseTenths(server.arg("value"));
        if (value < 100 || value > 300 || value % 5 != 0) {
            sendText(400, "目标温度范围为 10～30 °C，步长 0.5 °C。");
            return;
        }
        saveResult(saveTargetSetting(value));
    });
    server.on("/settings/schedule", HTTP_POST, []() {
        if (!requireAutomaticMode()) return;
        const int start = parseTime(server.arg("start"));
        const int end = parseTime(server.arg("end"));
        if (start < 0 || end < 0) {
            sendText(400, "请输入有效的起始时间和截止时间。");
            return;
        }
        saveResult(saveHeatingSchedule(start, end));
    });
    server.on("/settings/low-gear", HTTP_POST, []() {
        if (!requireAutomaticMode()) return;
        const int value = parseTenths(server.arg("value"));
        if (value < 5 || value > 30) {
            sendText(400, "低温加热档位范围为 0.5～3.0，步长 0.1。");
            return;
        }
        saveResult(saveLowHeatingGear(value));
    });
    server.on("/settings/mode", HTTP_POST, []() {
        if (!requireFormToken()) return;
        const String value = server.arg("value");
        if (value != "auto" && value != "manual") {
            sendText(400, "请选择手动控制或自动控制。");
            return;
        }
        saveResult(saveModeSetting(value == "manual"));
    });
    server.on("/control/gear", HTTP_POST, []() {
        if (!requireFormToken()) return;
        const PortalStatus status = readStatus();
        if (!status.manual || !status.calibrated) {
            sendText(409, "请选择手动控制，并在设备上完成舵机标定。");
            return;
        }
        const int value = parseTenths(server.arg("value"));
        if (value != 10 && value != 20 && value != 30) {
            sendText(400, "请选择 1、2 或 3 档。");
            return;
        }
        if (applyManualGear(value)) redirectHome();
        else sendText(500, "档位指令输出失败。");
    });
    server.on("/history", HTTP_GET, sendHistory);
    server.on("/nuanshi.log", HTTP_GET, []() {
        if (!requireLogin()) return;
        File log = openEventLog();
        if (!log) {
            sendText(readStatus().sdMounted ? 404 : 503, "当前没有可下载的 SD 日志。");
            return;
        }
        responseHeaders();
        server.sendHeader("Content-Disposition", "attachment; filename=nuanshi.log");
        server.streamFile(log, "text/plain; charset=utf-8");
    });
    server.on("/logs/preview", HTTP_GET, sendLogPreview);
    server.on("/logs/delete", HTTP_POST, []() {
        if (!requireFormToken()) return;
        if (!deleteEventLog()) {
            sendText(503, "删除失败，请检查 SD 卡。");
            return;
        }
        Serial.println("[SD] log cleared via web portal");
        redirectHome();
    });
    server.onNotFound([]() { sendText(404, "页面不存在。"); });
    server.begin();
}

void pollWebPortal() {
    if (session.length() && millis() - sessionStarted >= sessionLifetimeMs) {
        session = "";
        formToken = "";
    }
    server.handleClient();
}
