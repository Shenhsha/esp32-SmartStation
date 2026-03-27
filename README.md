# ESP32 智能环境监测站 v2.0

基于 ESP32 的多功能环境监测系统，支持传感器数据采集、内网 Web 实时显示、异常报警。

![ESP32](https://img.shields.io/badge/ESP32-WROOM--32-orange) ![Arduino](https://img.shields.io/badge/Arduino-IDE-blue) ![License](https://img.shields.io/badge/License-MIT-green)

## 功能

- **多传感器采集**：温度（NTC热敏电阻）、光照（光敏电阻）、人体感应（PIR）、超声波测距
- **Web 实时监控**：手机/电脑浏览器访问内网 IP，3秒自动刷新
- **异常报警**：蜂鸣器 + LED，支持布防/撤防切换
- **JSON API**：`/api` 接口返回传感器数据，方便对接 Home Assistant 等平台
- **模块化架构**：每个功能独立 namespace，易扩展

## 硬件

| 模块 | 引脚 |
|------|------|
| NTC 温度传感器 | GPIO 34 (ADC1) |
| 光敏电阻 | GPIO 35 (ADC1) |
| PIR 人体红外 | GPIO 14 |
| 超声波 Trig | GPIO 13 |
| 超声波 Echo | GPIO 12 |
| 有源蜂鸣器 | GPIO 16 |
| LED（220Ω电阻）| GPIO 5 |
| 按键 | GPIO 23 |

> 温度和光照使用 ADC1 引脚，避免与 WiFi 冲突。

## 使用

1. 修改代码中的 WiFi 配置：
```cpp
const char* WIFI_SSID     = "你的WiFi名";
const char* WIFI_PASSWORD = "WiFi密码";
```

2. 用 Arduino IDE 烧录（开发板选 ESP32 Dev Module）

3. 打开串口监视器（115200 baud），查看 IP 地址

4. 浏览器访问该 IP，即可看到监控面板

## API

`GET /api` 返回 JSON：
```json
{
  "temperature": 26.5,
  "light": 68,
  "distance": 120.0,
  "pir": 0,
  "alarm": true,
  "uptime": 3600
}
```

## 报警逻辑

| 状态 | LED | 蜂鸣器 |
|------|-----|--------|
| 温度 >35°C 或 距离 <30cm | 快闪 | 响 |
| 检测到人体 | 柔和亮 | 静音 |
| 光照 <20% | 中等亮 | 静音 |
| 正常 | 灭 | 静音 |

按键或网页按钮切换布防/撤防。

## 项目结构

```
SmartStation_modular.ino
├── Config      → WiFi/报警阈值/采样间隔
├── Pins        → 引脚定义
├── Sensors     → 传感器采集（含NTC Steinhart-Hart转换）
├── Alarm       → 报警逻辑 + LED + 蜂鸣器
├── Button      → 按键中断（防抖）
├── WebPage     → HTML 页面生成
├── Web         → 路由处理 + WebServer
└── Wifi        → 连接管理
```

## License

MIT
