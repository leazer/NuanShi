#include "web_portal.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "bench_storage.h"
#include "firmware_version.h"
#include "policy_package.h"
#include "target_temperature.h"
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
bool (*saveModeSetting)(ControlMode);
bool (*applyManualGear)(int);
bool (*installPolicy)(const HeatingPolicy&);
bool (*saveSampleIntervalSetting)(int);
const esp_partition_t* firmwarePartition = nullptr;
esp_ota_handle_t firmwareHandle = 0;
size_t firmwareBytes = 0;
size_t firmwareExpected = 0;
bool firmwareComplete = false;
int firmwareError = 400;

void abortFirmwareUpload() {
    if (firmwareHandle) esp_ota_abort(firmwareHandle);
    firmwareHandle = 0;
    firmwarePartition = nullptr;
}

constexpr char pageStart[] = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>暖时</title>
<style>
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;color:#e6eff5;background:#101923;margin:0}
main{max-width:720px;padding:24px 16px 48px;margin:auto}h1{margin:0 0 16px}h2{font-size:1.15rem;margin:0 0 14px}
section{background:#192734;border:1px solid #2c4150;border-radius:12px;padding:18px;margin-bottom:14px}
dl{display:grid;grid-template-columns:auto 1fr;gap:8px 20px;margin:0}dd{margin:0;overflow-wrap:anywhere}
dt,small{color:#b9c9d6}form{margin:0}form+form{border-top:1px solid #2c4150;padding-top:12px;margin-top:12px}
label{display:inline-flex;align-items:center;gap:8px;margin:0 8px 8px 0}
input,select,textarea,button,a.button{font:inherit;padding:8px 10px;border-radius:6px;border:1px solid #526a7b;color:inherit;background:#111d28}
input[type=number]{width:5.5rem}button,a.button{cursor:pointer;text-decoration:none;display:inline-block}
button:hover,a.button:hover{border-color:#7ed8e6}button.danger,.danger{color:#ff9c9a}.warn{color:#ffd18f}.good{color:#8fddd1}
fieldset{padding:0;border:0;margin:14px 0 0;min-width:0}fieldset:disabled,button:disabled{opacity:.55;cursor:default}
.actions{display:flex;gap:12px;flex-wrap:wrap}.actions form{display:inline}
#temperature-chart{display:block;width:100%;height:300px;background:#111d28;border-radius:6px;touch-action:pan-y}
.chart-key{display:flex;gap:8px 14px;flex-wrap:wrap;margin:0 0 10px;font-size:.83rem}
#chart-note{display:block;min-height:1.25rem;margin-top:8px}
#policy-package{display:block;width:100%;min-height:12rem;margin:8px 0;font:0.85rem ui-monospace,monospace}
#policy-file{max-width:100%}
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

bool authorizeFirmwareUpload() {
    const String supplied = server.header("X-Firmware-Token");
    return loggedIn() && formToken.length() == 32 && formToken.equalsConstantTime(supplied);
}

void receiveFirmwareUpload() {
    auto& upload = server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START: {
            if (firmwareHandle || firmwareComplete) {
                abortFirmwareUpload();
                firmwareComplete = false;
                firmwareError = 400;
                return;
            }
            firmwareBytes = firmwareExpected = 0;
            firmwareError = 400;
            if (!authorizeFirmwareUpload() || upload.name != "firmware") return;
            firmwarePartition = esp_ota_get_next_update_partition(nullptr);
            const String size = server.header("X-Firmware-Size");
            if (!firmwarePartition || !size.length() || size.length() > 7) return;
            for (unsigned i = 0; i < size.length(); ++i) {
                if (!isDigit(size[i])) return;
            }
            firmwareExpected = size.toInt();
            if (!firmwareExpected || firmwareExpected > firmwarePartition->size) return;
            firmwareError = 500;
            if (esp_ota_begin(firmwarePartition, firmwareExpected, &firmwareHandle) != ESP_OK) {
                abortFirmwareUpload();
            }
            break;
        }
        case UPLOAD_FILE_WRITE:
            if (!firmwareHandle) return;
            if (upload.currentSize > firmwareExpected - firmwareBytes) {
                firmwareError = 400;
                abortFirmwareUpload();
            } else if (esp_ota_write(firmwareHandle, upload.buf, upload.currentSize) != ESP_OK) {
                abortFirmwareUpload();
            } else {
                firmwareBytes += upload.currentSize;
            }
            break;
        case UPLOAD_FILE_END:
            if (!firmwareHandle) return;
            if (firmwareBytes != firmwareExpected || upload.totalSize != firmwareExpected) {
                firmwareError = 400;
                abortFirmwareUpload();
            } else {
                const esp_err_t result = esp_ota_end(firmwareHandle);
                firmwareHandle = 0;
                firmwareComplete = result == ESP_OK;
                firmwareError = 422;
            }
            break;
        case UPLOAD_FILE_ABORTED:
            abortFirmwareUpload();
            firmwareComplete = false;
            break;
    }
}

void finishFirmwareUpload() {
    const bool complete = firmwareComplete;
    firmwareComplete = false;
    if (!requireLogin()) {
        abortFirmwareUpload();
        return;
    }
    if (!authorizeFirmwareUpload()) {
        abortFirmwareUpload();
        sendText(403, "请求校验失败，请刷新升级页。");
        return;
    }
    if (!complete || !firmwarePartition) {
        abortFirmwareUpload();
        sendText(firmwareError, "固件上传失败，设备继续运行当前版本。");
        return;
    }
    if (esp_ota_set_boot_partition(firmwarePartition) != ESP_OK) {
        abortFirmwareUpload();
        sendText(500, "固件启动校验失败，设备继续运行当前版本。");
        return;
    }
    abortFirmwareUpload();
    appendLog("Firmware OTA verified; restarting", readStatus().epoch != 0);
    sendText(200, "固件校验通过，设备正在重启。约 20 秒后可返回首页。");
    delay(350);
    ESP.restart();
}

void showFirmwareUpdater() {
    if (!requireLogin()) return;
    const auto* current = esp_ota_get_running_partition();
    const auto* next = esp_ota_get_next_update_partition(nullptr);
    String html(pageStart);
    html += F("<section><h2>固件更新</h2><p>软件版本 ");
    html += firmwareVersion;
    html += F(" · 运行分区 ");
    html += current ? current->label : "未知";
    html += F("</p>");
    if (next) {
        html += F("<form id=firmware-form data-token=\"");
        html += formToken;
        html += F("\" data-limit=");
        html += String(next->size);
        html += F("><label>固件文件 <input id=firmware-file type=file accept=.bin required></label>"
                  "<button>上传并重启</button>"
                  "<p id=firmware-progress hidden><progress max=100 value=0 aria-label=\"固件上传进度\"></progress> "
                  "<span>0%</span></p><p id=firmware-state role=status></p></form>");
        html += F(R"HTML(<script>
const form = document.querySelector('#firmware-form');
const picker = document.querySelector('#firmware-file');
const state = document.querySelector('#firmware-state');
const progress = document.querySelector('#firmware-progress');
const bar = progress.querySelector('progress');
const percent = progress.querySelector('span');
const button = form.querySelector('button');
function setProgress(value) {
    bar.value = value;
    percent.textContent = value + '%';
}
function unlock() {
    button.disabled = false;
    picker.disabled = false;
}
form.addEventListener('submit', event => {
    event.preventDefault();
    if (button.disabled) return;
    const file = picker.files[0];
    if (!file || !file.size || file.size > Number(form.dataset.limit)) {
        state.textContent = '文件为空或超出分区容量。';
        return;
    }
    if (!confirm('安装该固件并重启设备？')) return;
    button.disabled = true;
    picker.disabled = true;
    progress.hidden = false;
    setProgress(0);
    state.textContent = '正在上传，请保持设备供电。';
    const request = new XMLHttpRequest();
    request.upload.onprogress = event => {
        if (event.lengthComputable) setProgress(Math.min(99, Math.floor(100 * event.loaded / event.total)));
    };
    request.upload.onload = () => {
        setProgress(100);
        state.textContent = '上传完成，设备正在校验固件。';
    };
    request.onload = () => {
        state.textContent = request.responseText || '设备未返回升级结果。';
        if (request.status !== 200) unlock();
    };
    request.onerror = request.onabort = () => {
        state.textContent = '连接中断。请稍后打开首页确认设备状态。';
        unlock();
    };
    request.open('POST', '/firmware');
    request.setRequestHeader('X-Firmware-Token', form.dataset.token);
    request.setRequestHeader('X-Firmware-Size', String(file.size));
    const data = new FormData();
    data.append('firmware', file);
    request.send(data);
});
</script>)HTML");
    } else {
        html += F("<p>未配置备用应用分区。</p>");
    }
    html += F("<a class=button href=/>返回首页</a></section></main></html>");
    responseHeaders();
    server.send(200, "text/html; charset=utf-8", html);
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
    const bool automatic = data.mode == ControlMode::Automatic;
    html += data.manual ? F("<dl data-mode=manual>") : automatic ? F("<dl data-mode=auto>") : F("<dl data-mode=observe>");
    html += F("<dt>控制模式</dt><dd>");
    html += data.manual ? "手动控制" : automatic ? "自动控制" : "观测模式";
    if (automatic) {
        html += F(" · ");
        html += data.autoState;
    }
    html += F("</dd><dt>策略版本</dt><dd>");
    html += validHeatingPolicy(data.policy) ? String(data.policy.revision) : String("无有效策略");
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
    html += F("</dd><dt>温度读取间隔</dt><dd>");
    html += String(data.sampleIntervalMinutes);
    html += F(" 分钟</dd><dt>室外温度</dt><dd>");
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
    html += F("<label>控制模式 <select name=value><option value=observe");
    if (data.mode == ControlMode::Observe) html += F(" selected");
    html += F(">观测模式</option><option value=manual");
    if (data.manual) html += F(" selected");
    html += F(">手动控制</option><option value=auto");
    if (data.mode == ControlMode::Automatic) html += F(" selected");
    html += F(">自动控制</option></select></label><button>保存</button></form>");
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
    html += F("<label>目标温度 <input type=number name=value min=");
    html += String(targetMinimumTenths / 10);
    html += F(" max=");
    html += String(targetMaximumTenths / 10);
    html += F(" step=");
    html += String(targetStepTenths / 10.0f, 1);
    html += F(" required value=");
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
    html += F("></label><button>保存</button></form></fieldset>");
    addForm(html, "/settings/sample-interval");
    html += F("<label>温度读取间隔 <input type=number name=value min=");
    html += String(mijiaMinIntervalMinutes);
    html += F(" max=");
    html += String(mijiaMaxIntervalMinutes);
    html += F(" step=1 required value=");
    html += String(data.sampleIntervalMinutes);
    html += F("> 分钟</label><button>保存</button></form></section>"
              "<section><h2>策略更新</h2>");
    addForm(html, "/settings/policy");
    html += F("<label>策略文件 <input id=policy-file type=file accept=\".json,application/json\"></label>"
              "<textarea id=policy-package name=package aria-label=\"策略内容\" required maxlength=4096 "
              "spellcheck=false>");
    if (validHeatingPolicy(data.policy)) html += encodePolicy(data.policy);
    html += F("</textarea><div class=actions><button>安装策略</button>"
              "<a class=button href=/policy.json>下载当前策略</a></div></form></section>"
              "<section><h2>固件</h2><a class=button href=/firmware>更新固件</a></section>"
              "<section><h2>温度曲线</h2><div class=chart-key>"
              "<span style=color:#79dfc4>室内温度 °C</span><span style=color:#93b6ff>室外温度 °C</span>"
              "<span style=color:#ffc885>手动档位</span><span style=color:#79dfc4>自动档位</span>"
              "<span style=color:#c7a8f1>测试档位</span></div>"
              "<canvas id=temperature-chart aria-label=\"最近 24 小时的室温、室外温度和档位指令\"></canvas>"
              "<small id=chart-note>正在读取温度曲线…</small></section>"
              "<section><h2>系统日志</h2><div class=actions><a class=button href=/nuanshi.log>下载日志</a>");
    addForm(html, "/logs/delete");
    html += F("<button class=danger onclick=\"return confirm('删除当前系统日志？此操作不可撤销。')\">删除日志</button>"
              "</form></div><pre id=log-preview tabindex=0 aria-label=\"最新系统日志\">正在读取日志…</pre>"
              "<small>预览文件末尾，完整记录可下载。</small></section><script>"
              "const policyFile=document.getElementById('policy-file');"
              "policyFile.addEventListener('change',async()=>{const file=policyFile.files[0];if(!file)return;"
              "if(file.size>4096){alert('策略文件不能超过 4 KB。');policyFile.value='';return;}"
              "try{document.getElementById('policy-package').value=await file.text()}catch{alert('无法读取策略文件。')}});"
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
    server.sendHeader("X-Indoor-Gap-Seconds", String(mijiaHistoryGapSeconds(status.sampleIntervalMinutes)));
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv; charset=utf-8", "");
    char chunk[768];
    size_t used = snprintf(chunk, sizeof(chunk), "%lu,%lu\n",
                           static_cast<unsigned long>(start), static_cast<unsigned long>(status.epoch));
    for (int i = 0; i < count; ++i) {
        const auto& row = records[i];
        if (row.epoch < start || row.epoch > status.epoch) continue;
        const bool gear = row.targetTenths == TemperatureRecord::gear || row.targetTenths == TemperatureRecord::testGear ||
                          row.targetTenths == TemperatureRecord::automaticGear;
        const char* type = row.targetTenths == TemperatureRecord::outdoor ? "outdoor" :
                           row.targetTenths == TemperatureRecord::gear ? "gear" :
                           row.targetTenths == TemperatureRecord::testGear ? "test" :
                           row.targetTenths == TemperatureRecord::automaticGear ? "auto" : "indoor";
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
                    bool (*mode)(ControlMode), bool (*manualGear)(int),
                    bool (*policy)(const HeatingPolicy&), bool (*sampleInterval)(int)) {
    readStatus = status;
    saveTargetSetting = target;
    saveHeatingSchedule = schedule;
    saveLowHeatingGear = lowGear;
    saveModeSetting = mode;
    applyManualGear = manualGear;
    installPolicy = policy;
    saveSampleIntervalSetting = sampleInterval;
    const char* headers[] = {"Cookie", "X-Firmware-Token", "X-Firmware-Size"};
    server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
    server.on("/", HTTP_GET, dashboard);
    server.on("/login", HTTP_POST, login);
    server.on("/firmware", HTTP_GET, showFirmwareUpdater);
    server.on("/firmware", HTTP_POST, finishFirmwareUpload, receiveFirmwareUpload);
    server.on("/status", HTTP_GET, []() {
        if (!requireLogin()) return;
        responseHeaders();
        server.send(200, "text/html; charset=utf-8", statusHtml(readStatus()));
    });
    server.on("/settings/target", HTTP_POST, []() {
        if (!requireAutomaticMode()) return;
        const int value = parseTenths(server.arg("value"));
        if (!validTargetTemperature(value)) {
            sendText(400, "目标温度范围为 23～26 °C，步长 0.5 °C。");
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
        if (value != "auto" && value != "manual" && value != "observe") {
            sendText(400, "请选择观测模式、手动控制或自动控制。");
            return;
        }
        if (value == "auto") {
            const auto data = readStatus();
            if (!data.calibrated || !validHeatingPolicy(data.policy)) {
                sendText(409, "自动控制需要舵机标定和有效策略。");
                return;
            }
        }
        saveResult(saveModeSetting(value == "manual" ? ControlMode::Manual :
                                   value == "auto" ? ControlMode::Automatic : ControlMode::Observe));
    });
    server.on("/settings/sample-interval", HTTP_POST, []() {
        if (!requireFormToken()) return;
        const int value = parseTenths(server.arg("value"));
        if (value % 10 != 0 || !validMijiaInterval(value / 10)) {
            sendText(400, "温度读取间隔须为 1 到 20 分钟之间的整数。");
            return;
        }
        saveResult(saveSampleIntervalSetting(value / 10));
    });
    server.on("/settings/policy", HTTP_POST, []() {
        if (!requireFormToken()) return;
        HeatingPolicy policy;
        if (!decodePolicy(server.arg("package"), policy)) {
            sendText(400, "策略格式或规则范围错误，当前策略仍有效。");
            return;
        }
        saveResult(installPolicy(policy));
    });
    server.on("/policy.json", HTTP_GET, []() {
        if (!requireLogin()) return;
        const auto policy = readStatus().policy;
        if (!validHeatingPolicy(policy)) {
            sendText(503, "没有可下载的策略。");
            return;
        }
        responseHeaders();
        server.sendHeader("Content-Disposition", "attachment; filename=nuanshi-policy.json");
        server.send(200, "application/json", encodePolicy(policy));
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
    // The multipart parser and route handler run in the same handleClient call.
    // Release an incomplete upload if parsing failed before reaching the route.
    if (firmwarePartition || firmwareHandle) abortFirmwareUpload();
    firmwareComplete = false;
}
