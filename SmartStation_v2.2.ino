//**********************************************************************
// SmartStation_modular.ino
// ESP32 智能环境监测站 v2.2 — LED独立 + 异步采集 + 温度校准
//**********************************************************************

#include <WiFi.h>
#include <WebServer.h>

// ==================== 配置 ====================
namespace Config {
  const char* WIFI_SSID     = "你的WiFi名";     // ← 改这里
  const char* WIFI_PASSWORD = "WiFi密码";        // ← 改这里
  const int   WIFI_TIMEOUT_MS = 30000;

  const float TEMP_ALARM_C    = 35.0;
  const float DIST_ALARM_CM   = 30.0;
  const float TEMP_CALIBRATE  = -10.0;  // 温度校准偏移（实测偏高则减）
  const int   READ_INTERVAL   = 500;    // 传感器采集间隔(ms)
  const int   SENSOR_TASK_CORE = 1;     // 传感器跑在核心1（Web跑核心0）
  const int   SENSOR_STACK_SIZE = 4096; // 传感器任务栈大小
}

// ==================== 引脚定义 ====================
namespace Pins {
  const int TEMP    = 34;
  const int LIGHT   = 35;
  const int PIR     = 14;
  const int TRIG    = 13;
  const int ECHO    = 12;
  const int BUZZER  = 16;
  const int LED     = 5;
  const int BUTTON  = 23;
}

namespace PwmCh {
  const int LED = 0;
}

// ==================== 数据模型 ====================
struct SensorData {
  float temperature;
  int   lightPercent;
  bool  pirDetected;
  float distanceCm;
  unsigned long uptimeMs;
};

struct EnvStatus {
  const char* level;
  const char* label;
  const char* cssClass;
  uint32_t    color;
};

EnvStatus classifyTemp(float t) {
  if (t > 35) return {"danger", "高温警告", "danger", 0xef4444};
  if (t > 30) return {"warn",   "偏高",     "warn",   0xf59e0b};
  return       {"normal", "正常",     "normal", 0x22c55e};
}

EnvStatus classifyLight(int p) {
  if (p < 20) return {"danger", "光线不足", "danger", 0xef4444};
  if (p < 40) return {"warn",   "偏暗",     "warn",   0xf59e0b};
  return       {"normal", "正常",     "normal", 0x22c55e};
}

EnvStatus classifyDist(float d) {
  if (d < 0)  return {"normal", "超出范围", "normal", 0x64748b};
  if (d < 30) return {"danger", "太近!",    "danger", 0xef4444};
  if (d < 50) return {"warn",   "注意",     "warn",   0xf59e0b};
  return       {"normal", "安全",     "normal", 0x22c55e};
}

// ==================== 异步传感器数据（跨核共享）====================
namespace SharedData {
  SensorData data;
  SemaphoreHandle_t mutex;

  void init() {
    mutex = xSemaphoreCreateMutex();
  }

  // 传感器任务写入
  void write(const SensorData& src) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      data = src;
      xSemaphoreGive(mutex);
    }
  }

  // Web/LED/Alarm 读取
  SensorData read() {
    SensorData copy;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      copy = data;
      xSemaphoreGive(mutex);
    }
    return copy;
  }
}

// ==================== 模块：传感器 ====================
namespace Sensors {
  // 温度：Steinhart-Hart + 校准偏移
  float readTemperature() {
    int adc = analogRead(Pins::TEMP);
    if (adc <= 0 || adc >= 4095) return -999;
    float voltage = adc * (3.3 / 4095.0);
    float Rt = 10000.0 * voltage / (3.3 - voltage);
    float tempK = 1.0 / (1.0 / 298.15 + log(Rt / 10000.0) / 3950.0);
    return (tempK - 273.15) + Config::TEMP_CALIBRATE;  // ← 加校准偏移
  }

  int readLightPercent() {
    int adc = analogRead(Pins::LIGHT);
    if (adc <= 0) return 0;
    if (adc >= 4095) return 100;
    float voltage = adc * (3.3 / 4095.0);
    float Rl = 10000.0 * voltage / (3.3 - voltage);
    float logR = log10(Rl);
    int percent = map((int)(logR * 100), 300, 600, 100, 0);
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

  void initPins() {
    pinMode(Pins::TEMP, INPUT);
    pinMode(Pins::LIGHT, INPUT);
    pinMode(Pins::PIR, INPUT);
    pinMode(Pins::TRIG, OUTPUT);
    pinMode(Pins::ECHO, INPUT);
  }

  // 异步采集函数（在 FreeRTOS task 中运行）
  void readAndPublish() {
    SensorData d;
    d.temperature  = readTemperature();
    d.lightPercent = readLightPercent();
    d.pirDetected  = readPIR();
    d.distanceCm   = readDistanceCm();
    d.uptimeMs     = millis();
    SharedData::write(d);
  }
}

// ==================== FreeRTOS 传感器任务 ====================
void sensorTask(void* pvParameters) {
  Serial.println("📡 传感器任务启动 (核心 " + String(xPortGetCoreID()) + ")");
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    Sensors::readAndPublish();
    // 精确周期：每 READ_INTERVAL ms 采集一次，不受执行时间影响
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(Config::READ_INTERVAL));
  }
}

// ==================== 模块：LED（独立于报警系统）====================
namespace LED {
  unsigned long lastBlink = 0;
  bool blinkState = false;

  void init() {
    ledcSetup(PwmCh::LED, 1000, 8);
    ledcAttachPin(Pins::LED, PwmCh::LED);
  }

  const char* update(const SensorData& d) {
    bool tempDanger = (d.temperature > Config::TEMP_ALARM_C);
    bool distDanger = (d.distanceCm > 0 && d.distanceCm < Config::DIST_ALARM_CM);

    if (tempDanger || distDanger) {
      if (millis() - lastBlink > 200) {
        lastBlink = millis();
        blinkState = !blinkState;
      }
      ledcWrite(PwmCh::LED, blinkState ? 255 : 0);
      return "danger";
    }

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

  void errorBlink() {
    for (int i = 0; i < 6; i++) {
      ledcWrite(PwmCh::LED, 255); delay(300);
      ledcWrite(PwmCh::LED, 0);   delay(300);
    }
  }
}

// ==================== 模块：报警（仅蜂鸣器）====================
namespace Alarm {
  bool enabled = true;

  const char* check(const SensorData& d) {
    bool tempDanger = (d.temperature > Config::TEMP_ALARM_C);
    bool distDanger = (d.distanceCm > 0 && d.distanceCm < Config::DIST_ALARM_CM);

    if (!enabled) {
      digitalWrite(Pins::BUZZER, LOW);
      return "disabled";
    }

    if (tempDanger || distDanger) {
      digitalWrite(Pins::BUZZER, HIGH);
      return "danger";
    }

    digitalWrite(Pins::BUZZER, LOW);
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

  String colorStr(uint32_t c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%06x", c & 0xFFFFFF);
    return buf;
  }

  String build(const SensorData& data, bool alarmOn, const char* ip, const char* ledStatus) {
    unsigned long s = data.uptimeMs / 1000;
    char uptime[12];
    snprintf(uptime, sizeof(uptime), "%02d:%02d:%02d", (int)(s/3600), (int)((s%3600)/60), (int)(s%60));

    EnvStatus ts = classifyTemp(data.temperature);
    EnvStatus ls = classifyLight(data.lightPercent);
    EnvStatus ds = classifyDist(data.distanceCm);

    String tVal = String(data.temperature, 1) + "°C";
    String lVal = String(data.lightPercent) + "%";
    String dVal = data.distanceCm < 0 ? "--" : String(data.distanceCm, 0) + "cm";

    String ac = alarmOn ? "#22c55e" : "#ef4444";
    const char* at = alarmOn ? "已布防" : "已撤防";
    const char* ab = alarmOn ? "点击撤防" : "点击布防";
    const char* ai = alarmOn ? "🛡️" : "🔓";

    const char* dot = data.pirDetected ? "on" : "off";
    const char* pir = data.pirDetected ? "检测到人体" : "无人";

    bool isDanger = (strcmp(ledStatus, "danger") == 0);
    String ledColor = isDanger ? "#ef4444" : (strcmp(ledStatus, "normal") != 0 ? "#38bdf8" : "#64748b");

    String h;
    h.reserve(4000);
    h += F("<!DOCTYPE html><html lang='zh'><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>ESP32 环境监测站</title>"
           "<style>"
           "*{margin:0;padding:0;box-sizing:border-box}"
           "body{font-family:-apple-system,'Microsoft YaHei',sans-serif;"
           "background:linear-gradient(135deg,#0f172a,#1e293b);color:#e2e8f0;"
           "min-height:100vh;padding:30px 15px}"
           ".c{max-width:700px;margin:0 auto}"
           "h1{text-align:center;font-size:1.6em;color:#38bdf8;margin-bottom:8px}"
           ".sub{text-align:center;color:#64748b;font-size:.85em;margin-bottom:25px}"
           ".g{display:grid;grid-template-columns:1fr 1fr;gap:15px;margin-bottom:15px}"
           "@media(max-width:500px){.g{grid-template-columns:1fr}}"
           ".cd{background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);"
           "border-radius:14px;padding:22px;text-align:center}"
           ".ic{font-size:2em;margin-bottom:6px}"
           ".lb{font-size:.8em;color:#94a3b8;margin-bottom:4px}"
           ".vl{font-size:2.2em;font-weight:700}"
           ".s{font-size:.78em;margin-top:6px;padding:3px 10px;border-radius:12px;display:inline-block}"
           ".s.normal{background:rgba(34,197,94,.15);color:#22c55e}"
           ".s.warn{background:rgba(245,158,11,.15);color:#f59e0b}"
           ".s.danger{background:rgba(239,68,68,.15);color:#ef4444}"
           ".bar{width:100%;height:6px;background:rgba(255,255,255,.08);"
           "border-radius:3px;margin-top:12px;overflow:hidden}"
           ".bf{height:100%;border-radius:3px;transition:width .5s}"
           ".dot{display:inline-block;width:12px;height:12px;border-radius:50%;"
           "margin-right:6px;vertical-align:middle}"
           ".dot.on{background:#38bdf8;box-shadow:0 0 8px #38bdf8}"
           ".dot.off{background:#475569}"
           ".ab{background:rgba(255,255,255,.05);border-radius:12px;"
           "padding:15px;text-align:center;margin-bottom:15px;"
           "display:flex;align-items:center;justify-content:center;gap:12px;flex-wrap:wrap}"
           ".btn{background:none;border:1px solid;border-radius:8px;"
           "padding:6px 16px;cursor:pointer;font-size:.85em;color:inherit}"
           ".btn:hover{opacity:.7}"
           ".ft{text-align:center;color:#475569;font-size:.75em;margin-top:20px}"
           "</style></head><body><div class='c'>");

    h += F("<h1>🌡️ ESP32 环境监测站</h1>");
    h += F("<div class='sub'>运行 ");
    h += uptime;
    h += F(" | ");
    h += ip;
    h += F("</div>");

    h += F("<div class='ab'>");
    h += F("<span>"); h += ai; h += F("</span>");
    h += F("<span style='color:"); h += ac; h += F(";'>"); h += at; h += F("</span>");
    h += F("<button class='btn' style='border-color:"); h += ac;
    h += F(";color:"); h += ac;
    h += F(";' onclick=\"fetch('/alarm').then(()=>location.reload())\">");
    h += ab; h += F("</button>");
    h += F("</div>");

    h += F("<div class='g'>");

    // 温度
    h += F("<div class='cd'><div class='ic'>🌡️</div><div class='lb'>温度</div>");
    h += F("<div class='vl' style='color:"); h += colorStr(ts.color); h += F(";'>");
    h += tVal; h += F("</div>");
    h += F("<div class='s "); h += ts.cssClass; h += F("'>"); h += ts.label; h += F("</div></div>");

    // 光照
    h += F("<div class='cd'><div class='ic'>💡</div><div class='lb'>光照</div>");
    h += F("<div class='vl' style='color:"); h += colorStr(ls.color); h += F(";'>");
    h += lVal; h += F("</div>");
    h += F("<div class='bar'><div class='bf' style='width:");
    h += String(data.lightPercent); h += F("%;background:");
    h += colorStr(ls.color); h += F(";'></div></div>");
    h += F("<div class='s "); h += ls.cssClass; h += F("'>"); h += ls.label; h += F("</div></div>");

    // 距离
    h += F("<div class='cd'><div class='ic'>📏</div><div class='lb'>距离</div>");
    h += F("<div class='vl' style='color:"); h += colorStr(ds.color); h += F(";'>");
    h += dVal; h += F("</div>");
    h += F("<div class='s "); h += ds.cssClass; h += F("'>"); h += ds.label; h += F("</div></div>");

    // 人体感应
    h += F("<div class='cd'><div class='ic'>👤</div><div class='lb'>人体感应</div>");
    h += F("<div class='vl' style='color:");
    h += (data.pirDetected ? "#38bdf8" : "#64748b");
    h += F(";'>");
    h += F("<span class='dot "); h += dot; h += F("'></span>");
    h += pir; h += F("</div></div>");

    h += F("</div>");
    h += F("<div class='ft'>🔄 3秒自动刷新 | API: <a href='/api' style='color:#38bdf8'>/api</a></div>");
    h += F("</div><script>setTimeout(()=>location.reload(),3000)</script></body></html>");

    return h;
  }
}

// ==================== 模块：Web 服务器 ====================
namespace Web {
  WebServer server(80);

  void handleRoot() {
    SensorData d = SharedData::read();
    const char* ip = WiFi.localIP().toString().c_str();
    const char* ledStatus = LED::update(d);
    server.send(200, "text/html", WebPage::build(d, Alarm::enabled, ip, ledStatus));
  }

  void handleAPI() {
    SensorData d = SharedData::read();
    char json[320];
    snprintf(json, sizeof(json),
      "{\"temperature\":%.1f,\"light\":%d,\"distance\":%.1f,\"pir\":%d,"
      "\"alarm\":%s,\"led\":\"%s\",\"uptime\":%lu}",
      d.temperature,
      d.lightPercent,
      d.distanceCm < 0 ? -1 : d.distanceCm,
      d.pirDetected ? 1 : 0,
      Alarm::enabled ? "true" : "false",
      LED::update(d),
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
  Serial.println("🔧 SmartStation v2.2 启动中...");

  SharedData::init();
  Sensors::initPins();
  LED::init();
  Button::init();

  // 启动异步传感器任务（跑在核心1，不影响核心0的Web）
  xTaskCreatePinnedToCore(
    sensorTask,               // 任务函数
    "SensorTask",             // 任务名
    Config::SENSOR_STACK_SIZE,// 栈大小
    NULL,                     // 参数
    1,                        // 优先级（1=低，正常运行即可）
    NULL,                     // 任务句柄（不需要）
    Config::SENSOR_TASK_CORE  // 绑定核心1
  );

  bool wifiOk = Wifi::connect();
  if (!wifiOk) {
    Serial.println("⚠️ 无WiFi，传感器和LED仍可工作");
    LED::errorBlink();
  } else {
    Web::init();
  }

  digitalWrite(Pins::BUZZER, HIGH); delay(100); digitalWrite(Pins::BUZZER, LOW);
  Serial.println("🌐 SmartStation v2.2 就绪");
}

// ==================== 主循环（核心0：只管Web+报警+LED）====================
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Web::loop();
  }

  SensorData d = SharedData::read();
  LED::update(d);
  Alarm::check(d);

  delay(1);  // 避免空转吃CPU
}
