# ESP32 SmartStation v2.2

智能环境监测站 — NTC温度 / 光照 / PIR人体感应 / 超声波测距 + WiFi Web面板 + JSON API

## 硬件

- 开发板: ESP32 (任意型号)
- NTC热敏电阻 (MF52AT, B=3950, R25=10kΩ)
- 光敏电阻 (分压电路, 暗阻1MΩ/亮阻1kΩ)
- PIR人体红外传感器
- HC-SR04超声波模块
- 有源蜂鸣器
- LED (PWM)
- 按键 (中断)

## 功能

- 4传感器实时采集 (500ms间隔)
- WiFi Web暗色主题面板 (3秒自动刷新)
- JSON API (`/api`)
- 远程布防/撤防 (`/alarm`)
- 按键物理切换报警状态
- LED环境指示灯 (独立于报警系统)

## 更新记录

### v2.2 (2026-03-28)
- **异步采集**: 传感器读取迁移到FreeRTOS任务, 跑在Core1, 不阻塞Core0的Web服务
- **温度校准**: 新增 `TEMP_CALIBRATE` 偏移量, 修正ESP32 ADC偏差 (-10°C)
- **共享数据**: 传感器数据通过mutex跨核安全读写 (`SharedData` 模块)
- **LED独立**: LED模块完全脱离报警系统, 直接根据环境状态响应, 不受布防/撤防影响
- **报警精简**: Alarm模块只控制蜂鸣器, 逻辑更清晰
- **状态分类**: 统一的 `EnvStatus` 结构体, 消除颜色/状态/样式三处重复代码
- **响应式布局**: 新增 `@media(max-width:500px)` 手机端单列适配
- **API补全**: `/api` 新增 `led` 字段, 返回当前LED状态
- **WiFi容错**: 连接失败不再死循环, LED闪6次后继续运行传感器和LED
- **代码清理**: 删除未使用的函数, 降低String对象创建次数

### v2.1 (2026-03-28) [废弃]
- LED从Alarm独立 (v2.2进一步完善)

### v2.0 (2026-03-27)
- 初始模块化重构版
- namespace模块划分: Config/Pins/Sensors/Alarm/Button/WebPage/Web/Wifi
- Steinhart-Hart温度转换
- Web暗色主题UI
- 按键中断布防/撤防

## 引脚

| 功能 | 引脚 | 类型 |
|------|------|------|
| 温度 (NTC) | GPIO34 | ADC1 |
| 光照 | GPIO35 | ADC1 |
| PIR | GPIO14 | Digital |
| 超声波 Trig | GPIO13 | Digital |
| 超声波 Echo | GPIO12 | Digital |
| 蜂鸣器 | GPIO16 | Digital |
| LED | GPIO5 | PWM |
| 按键 | GPIO23 | Digital+INT |

## 校准

温度偏高/偏低时修改 `Config::TEMP_CALIBRATE`:
- 读数比实际高10°C → 设为 `-10.0`
- 读数比实际低5°C → 设为 `5.0`

## API

```
GET /          → Web面板
GET /api       → JSON数据
GET /alarm     → 切换布防/撤防
```

JSON 响应示例:
```json
{
  "temperature": 25.3,
  "light": 60,
  "distance": 120.0,
  "pir": 0,
  "alarm": true,
  "led": "normal",
  "uptime": 3600
}
```
