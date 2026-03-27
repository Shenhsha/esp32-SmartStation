//**********************************************************************
// SmartStation_modular.ino
// ESP32 智能环境监测站 v2.0 — 模块化重构版
//**********************************************************************

#include <WiFi.h>
#include <WebServer.h>

// ==================== 配置 ====================
namespace Config {
  const char* WIFI_SSID     = "zhang";     // ← 改这里
  const char* WIFI_PASSWORD = "Zhangjinhua1975217";        // ← 改这里
  const int   WIFI_TIMEOUT_MS = 30000;           // 30秒连接超时

  const float TEMP_ALARM_C  = 35.0;   // 温度报警阈值
  const float DIST_ALARM_CM = 30.0;   // 距离报警阈值
  const int   READ_INTERVAL = 500;    // 传感器读取间隔(ms)
}

// ==================== 引脚定义 ====================
namespace Pins {
  const int TEMP    = 34;   // 温度传感器 (ADC1, 与WiFi不冲突)
  const int LIGHT   = 35;   // 光敏电阻 (ADC1, 与WiFi不冲突)
  const int PIR     = 14;   // 人体红外 (Digital)
  const int TRIG    = 13;   // 超声波 Trig
  const int ECHO    = 12;   // 超声波 Echo
  const int BUZZER  = 16;   // 蜂鸣器
  const int LED     = 5;    // LED (PWM)
  const int BUTTON  = 23;   // 按键
}

namespace PwmCh {
  const int LED = 0;
}

// ==================== 数据模型 ====================
struct SensorData {
  float temperature;      // °C
  int   lightPercent;     // 0-100%
  bool  pirDetected;      // 人体感应
  float distanceCm;       // 超声波距离, -1=超时
  unsigned long uptimeMs; // 运行时间
};

// ==================== 模块：传感器 ====================
namespace Sensors {
  SensorData data;

  // NTC热敏电阻 → 温度 (Steinhart-Hart方程)
  // MF52AT: B值≈3950, R25=10kΩ, 分压电阻10kΩ接3.3V
  float readTemperature() {
    int adc = analogRead(Pins::TEMP);
    if (adc <= 0 || adc >= 4095) return -999;  // 接线异常

    // ADC → 电压 → 热敏电阻阻值
    float voltage = adc * (3.3 / 4095.0);
    float Rt = 10000.0 * voltage / (3.3 - voltage);  // 分压公式

    // Steinhart-Hart 简化(B值法)
    // T = 1 / (1/T0 + ln(Rt/R0)/B) - 273.15
    const float B = 3950.0;     // B值
    const float R0 = 10000.0;   // 25°C时阻值
    const float T0 = 298.15;    // 25°C = 298.15K
    float tempK = 1.0 / (1.0 / T0 + log(Rt / R0) / B);
    return tempK - 273.15;  // 转摄氏度
  }

  // 光敏电阻 → 光照百分比 (对数映射)
  // 分压电路同温度传感器，暗阻≈1MΩ，亮阻≈1kΩ
  int readLightPercent() {
    int adc = analogRead(Pins::LIGHT);
    if (adc <= 0) return 0;
    if (adc >= 4095) return 100;

    float voltage = adc * (3.3 / 4095.0);
    float Rl = 10000.0 * voltage / (3.3 - voltage);  // 光敏电阻阻值

    // 对数映射：暗阻1MΩ→0%，亮阻1kΩ→100%
    // log10(1000)=3, log10(1000000)=6
    float logR = log10(Rl);
    int percent = map((int)(logR * 100), 300, 600, 100, 0);  // 反比：阻值越小越亮
    return constrain(percent, 0, 100);
  }

  bool readPIR() {
    return digitalRead(Pins::PIR) == HIGH;
  }

  float readDistanceCm() {
    digitalWrite(Pins::TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(Pins::TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(Pins::TRIG, LOW);
    unsigned long duration = pulseIn(Pins::ECHO, HIGH, 30000);
    if (duration == 0) return -1;
    return duration / 58.0;
  }

  void init() {
    pinMode(Pins::TEMP, INPUT);
    pinMode(Pins::LIGHT, INPUT);
    pinMode(Pins::PIR, INPUT);
    pinMode(Pins::TRIG, OUTPUT);
    pinMode(Pins::ECHO, INPUT);
  }

  void readAll() {
    data.temperature  = readTemperature();
    data.lightPercent = readLightPercent();
    data.pirDetected  = readPIR();
    data.distanceCm   = readDistanceCm();
    data.uptimeMs     = millis();
  }
}

// ==================== 模块：报警 ====================
namespace Alarm {
  bool enabled = true;
  bool active  = false;
  unsigned long lastBlink = 0;
  bool ledState = false;

  void init() {
    pinMode(Pins::BUZZER, OUTPUT);
    ledcSetup(PwmCh::LED, 1000, 8);
    ledcAttachPin(Pins::LED, PwmCh::LED);
  }

  // 报警状态（供 Web 和其他模块查询）
  // 返回: "normal" / "pir" / "dark" / "danger"
  const char* check() {
    if (!enabled) {
      digitalWrite(Pins::BUZZER, LOW);
      ledcWrite(PwmCh::LED, 0);
      active = false;
      return "disabled";
    }

    const SensorData& d = Sensors::data;
    bool tempAlarm = (d.temperature > Config::TEMP_ALARM_C);
    bool distAlarm = (d.distanceCm > 0 && d.distanceCm < Config::DIST_ALARM_CM);

    if (tempAlarm || distAlarm) {
      // 危险：蜂鸣 + 闪灯
      digitalWrite(Pins::BUZZER, HIGH);
      if (millis() - lastBlink > 200) {
        lastBlink = millis();
        ledState = !ledState;
        ledcWrite(PwmCh::LED, ledState ? 255 : 0);
      }
      active = true;
      return "danger";
    }

    active = false;
    digitalWrite(Pins::BUZZER, LOW);

    if (d.pirDetected) {
      ledcWrite(PwmCh::LED, 80);
      return "pir";
    }
    if (d.lightPercent < 20) {
      ledcWrite(PwmCh::LED, 120);
      return "dark";
    }

    ledcWrite(PwmCh::LED, 0);
    return "normal";
  }

  void toggle() { enabled = !enabled; }
}

// ==================== 模块：按键 ====================
namespace Button {
  void IRAM_ATTR isr() {
    static unsigned long lastTime = 0;
    unsigned long now = millis();
    if (now - lastTime > 300) {
      lastTime = now;
      Alarm::toggle();
    }
  }

  void init() {
    pinMode(Pins::BUTTON, INPUT);
    attachInterrupt(digitalPinToInterrupt(Pins::BUTTON), isr, FALLING);
  }
}

// ==================== 模块：Web 页面 ====================
namespace WebPage {
  String statusLabel(const char* type) {
    if (strcmp(type, "danger") == 0) return R"(<span class="s danger">危险</span>)";
    if (strcmp(type, "pir") == 0)    return R"(<span class="s normal">有人</span>)";
    if (strcmp(type, "dark") == 0)   return R"(<span class="s warn">偏暗</span>)";
    return R"(<span class="s normal">正常</span>)";
  }

  String tempColor(float t) { return t > 35 ? "#ef4444" : (t > 30 ? "#f59e0b" : "#22c55e"); }
  String tempStatus(float t) { return t > 35 ? "高温警告" : (t > 30 ? "偏高" : "正常"); }
  String tempClass(float t)  { return t > 35 ? "danger" : (t > 30 ? "warn" : "normal"); }

  String lightColor(int p) { return p < 20 ? "#ef4444" : (p < 40 ? "#f59e0b" : "#22c55e"); }
  String lightStatus(int p) { return p < 20 ? "光线不足" : (p < 40 ? "偏暗" : "正常"); }
  String lightClass(int p)  { return p < 20 ? "danger" : (p < 40 ? "warn" : "normal"); }

  String distColor(float d) {
    if (d < 0) return "#64748b";
    return d < 30 ? "#ef4444" : (d < 50 ? "#f59e0b" : "#22c55e");
  }
  String distStatus(float d) {
    if (d < 0) return "超出范围";
    return d < 30 ? "太近!" : (d < 50 ? "注意" : "安全");
  }
  String distClass(float d) {
    if (d < 0) return "normal";
    return d < 30 ? "danger" : (d < 50 ? "warn" : "normal");
  }

  String build(const SensorData& data, bool alarmOn, const char* ip) {
    unsigned long s = data.uptimeMs / 1000;
    char uptime[12];
    sprintf(uptime, "%02d:%02d:%02d", (int)(s/3600), (int)((s%3600)/60), (int)(s%60));

    String t = String(data.temperature, 1);
    String l = String(data.lightPercent);
    String d = data.distanceCm < 0 ? "--" : String(data.distanceCm, 0) + "cm";

    String ac = alarmOn ? "#22c55e" : "#ef4444";
    String at = alarmOn ? "已布防" : "已撤防";
    String ab = alarmOn ? "点击撤防" : "点击布防";
    String ai = alarmOn ? "🛡️" : "🔓";

    String dot = data.pirDetected ? "on" : "off";
    String pir = data.pirDetected ? "检测到人体" : "无人";

    String h;
    h.reserve(3500);
    h  = F("<!DOCTYPE html><html lang='zh'><head><meta charset='UTF-8'>");
    h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    h += F("<title>ESP32 环境监测站</title>");
    h += F("<style>*{margin:0;padding:0;box-sizing:border-box}");
    h += F("body{font-family:-apple-system,'Microsoft YaHei',sans-serif;");
    h += F("background:linear-gradient(135deg,#0f172a,#1e293b);color:#e2e8f0;");
    h += F("min-height:100vh;padding:30px 15px}");
    h += F(".c{max-width:700px;margin:0 auto}");
    h += F("h1{text-align:center;font-size:1.6em;color:#38bdf8;margin-bottom:8px}");
    h += F(".sub{text-align:center;color:#64748b;font-size:.85em;margin-bottom:25px}");
    h += F(".g{display:grid;grid-template-columns:1fr 1fr;gap:15px;margin-bottom:15px}");
    h += F(".cd{background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);");
    h += F("border-radius:14px;padding:22px;text-align:center}");
    h += F(".ic{font-size:2em;margin-bottom:6px}");
    h += F(".lb{font-size:.8em;color:#94a3b8;margin-bottom:4px}");
    h += F(".vl{font-size:2.2em;font-weight:700}");
    h += F(".s{font-size:.78em;margin-top:6px;padding:3px 10px;border-radius:12px;display:inline-block}");
    h += F(".s.normal{background:rgba(34,197,94,.15);color:#22c55e}");
    h += F(".s.warn{background:rgba(245,158,11,.15);color:#f59e0b}");
    h += F(".s.danger{background:rgba(239,68,68,.15);color:#ef4444}");
    h += F(".bar{width:100%;height:6px;background:rgba(255,255,255,.08);");
    h += F("border-radius:3px;margin-top:12px;overflow:hidden}");
    h += F(".bf{height:100%;border-radius:3px;transition:width .5s}");
    h += F(".dot{display:inline-block;width:12px;height:12px;border-radius:50%;");
    h += F("margin-right:6px;vertical-align:middle}");
    h += F(".dot.on{background:#38bdf8;box-shadow:0 0 8px #38bdf8}");
    h += F(".dot.off{background:#475569}");
    h += F(".ab{background:rgba(255,255,255,.05);border-radius:12px;");
    h += F("padding:15px;text-align:center;margin-bottom:15px;");
    h += F("display:flex;align-items:center;justify-content:center;gap:12px}");
    h += F(".btn{background:none;border:1px solid;border-radius:8px;");
    h += F("padding:6px 16px;cursor:pointer;font-size:.85em;color:inherit}");
    h += F(".btn:hover{opacity:.7}");
    h += F(".ft{text-align:center;color:#475569;font-size:.75em;margin-top:20px}");
    h += F("</style></head><body><div class='c'>");
    h += F("<h1>🌡️ ESP32 环境监测站</h1>");
    h += F("<div class='sub'>运行 "); h += uptime; h += F(" | "); h += ip; h += F("</div>");
    h += F("<div class='ab'><span>"); h += ai; h += F("</span>");
    h += F("<span style='color:"); h += ac; h += F(";'>"); h += at; h += F("</span>");
    h += F("<button class='btn' style='border-color:"); h += ac; h += F(";color:"); h += ac;
    h += F(";' onclick=\"fetch('/alarm').then(()=>location.reload())\">"); h += ab; h += F("</button></div>");
    h += F("<div class='g'>");
    // 温度
    h += F("<div class='cd'><div class='ic'>🌡️</div><div class='lb'>温度</div>");
    h += F("<div class='vl' style='color:"); h += tempColor(data.temperature); h += F(";'>"); h += t; h += F("°C</div>");
    h += F("<div class='s "); h += tempClass(data.temperature); h += F("'>"); h += tempStatus(data.temperature); h += F("</div></div>");
    // 光照
    h += F("<div class='cd'><div class='ic'>💡</div><div class='lb'>光照</div>");
    h += F("<div class='vl' style='color:"); h += lightColor(data.lightPercent); h += F(";'>"); h += l; h += F("%</div>");
    h += F("<div class='bar'><div class='bf' style='width:"); h += l; h += F("%;background:");
    h += lightColor(data.lightPercent); h += F(";'></div></div>");
    h += F("<div class='s "); h += lightClass(data.lightPercent); h += F("'>"); h += lightStatus(data.lightPercent); h += F("</div></div>");
    // 距离
    h += F("<div class='cd'><div class='ic'>📏</div><div class='lb'>距离</div>");
    h += F("<div class='vl' style='color:"); h += distColor(data.distanceCm); h += F(";'>"); h += d; h += F("</div>");
    h += F("<div class='s "); h += distClass(data.distanceCm); h += F("'>"); h += distStatus(data.distanceCm); h += F("</div></div>");
    // 人体感应
    h += F("<div class='cd'><div class='ic'>👤</div><div class='lb'>人体感应</div>");
    h += F("<div class='vl' style='color:"); h += (data.pirDetected ? "#38bdf8" : "#64748b"); h += F(";'>");
    h += F("<span class='dot "); h += dot; h += F("'></span>"); h += pir; h += F("</div></div>");
    h += F("</div><div class='ft'>🔄 3秒自动刷新 | API: <a href='/api' style='color:#38bdf8'>/api</a></div>");
    h += F("</div><script>setTimeout(()=>location.reload(),3000)</script></body></html>");
    return h;
  }
}

// ==================== 模块：Web 服务器 ====================
namespace Web {
  WebServer server(80);

  void handleRoot() {
    const char* ip = WiFi.localIP().toString().c_str();
    server.send(200, "text/html", WebPage::build(Sensors::data, Alarm::enabled, ip));
  }

  void handleAPI() {
    char json[256];
    snprintf(json, sizeof(json),
      "{\"temperature\":%.1f,\"light\":%d,\"distance\":%.1f,\"pir\":%d,\"alarm\":%s,\"uptime\":%lu}",
      Sensors::data.temperature,
      Sensors::data.lightPercent,
      Sensors::data.distanceCm < 0 ? -1 : Sensors::data.distanceCm,
      Sensors::data.pirDetected ? 1 : 0,
      Alarm::enabled ? "true" : "false",
      millis() / 1000
    );
    server.send(200, "application/json", json);
  }

  void handleAlarm() {
    Alarm::toggle();
    server.send(200, "text/plain", Alarm::enabled ? "ALARM_ON" : "ALARM_OFF");
  }

  void handleNotFound() {
    server.send(404, "text/plain", "404 Not Found");
  }

  void init() {
    server.on("/", handleRoot);
    server.on("/api", handleAPI);
    server.on("/alarm", handleAlarm);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("🚀 Web 服务器启动成功");
  }

  void loop() {
    server.handleClient();
  }
}

// ==================== 模块：WiFi ====================
namespace Wifi {
  bool connect() {
    Serial.printf("连接 WiFi: %s ", Config::WIFI_SSID);
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - start > Config::WIFI_TIMEOUT_MS) {
        Serial.println("\n❌ WiFi 连接超时！");
        return false;
      }
      delay(500);
      Serial.print(".");
    }
    Serial.printf("\n✅ 已连接！IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
}

// ==================== 初始化 ====================
void setup() {
  Serial.begin(115200);

  Sensors::init();
  Alarm::init();
  Button::init();

  if (!Wifi::connect()) {
    // 连不上 WiFi → LED 快闪提示
    while (true) {
      ledcWrite(PwmCh::LED, 255); delay(200);
      ledcWrite(PwmCh::LED, 0);   delay(200);
    }
  }

  Web::init();

  // 开机提示音
  digitalWrite(Pins::BUZZER, HIGH); delay(100); digitalWrite(Pins::BUZZER, LOW);
  Serial.println("🌐 浏览器打开上面的 IP 地址查看监控面板");
}

// ==================== 主循环 ====================
static unsigned long lastRead = 0;

void loop() {
  Web::loop();

  if (millis() - lastRead >= Config::READ_INTERVAL) {
    lastRead = millis();
    Sensors::readAll();
    Alarm::check();
  }
}
