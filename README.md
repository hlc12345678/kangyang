# 康养智能药盒（kangyang）

基于 **ESP32-S3 N16R8** 的智能药盒项目，集成火山引擎豆包大模型 AI 对话能力。

## 硬件规格

| 项目 | 参数 |
|------|------|
| 芯片 | ESP32-S3 |
| Flash | 16 MB |
| PSRAM | 8 MB（Octal SPI，80 MHz） |
| 开发框架 | ESP-IDF v5.5.2 |

## 功能特性

- 📡 **Wi-Fi 连接**：Station 模式，支持自动重连
- 🤖 **豆包 AI 对话**：通过 HTTP POST 调用火山引擎豆包大模型 REST API
- 🔒 **HTTPS 安全连接**：使用 ESP-IDF 内置证书包（`esp_crt_bundle_attach`）
- 💾 **PSRAM 接收缓冲**：128 KB 接收缓冲区分配在外部 PSRAM，支持处理大模型长回复
- 📦 **cJSON 解析**：自动提取 `choices[0].message.content` 中的文字内容
- 🎯 **FreeRTOS 任务**：AI 对话运行在独立任务中，不阻塞主程序

## 项目结构

```
kangyang/
├── CMakeLists.txt          # 顶层 CMake 配置
├── sdkconfig.defaults      # 默认 SDK 配置（PSRAM/HTTPS/Wi-Fi 等）
├── README.md
└── main/
    ├── CMakeLists.txt      # main 组件 CMake 配置
    ├── Kconfig.projbuild   # menuconfig 用户配置项
    ├── main.c              # 应用主入口（NVS + Wi-Fi + 任务启动）
    ├── doubao_api.h        # 豆包 API 接口声明与配置宏
    └── doubao_api.c        # 豆包 API 完整实现
```

## 快速上手

### 1. 安装 ESP-IDF v5.5.2

参考 [ESP-IDF 安装指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html)。

### 2. 克隆并配置项目

```bash
git clone https://github.com/hlc12345678/kangyang.git
cd kangyang
```

### 3. 修改 API Key 和 Wi-Fi 配置

在 `main/doubao_api.h` 中修改以下宏：

```c
#define DOUBAO_API_KEY  "your-real-api-key"   // 火山引擎 API Key
#define DOUBAO_MODEL_ID "doubao-1-5-pro-32k-250115"  // 模型推理接入点 ID
```

通过 `menuconfig` 配置 Wi-Fi：

```bash
idf.py menuconfig
# 进入 "智能药盒配置 (Kangyang)" → "Wi-Fi 配置"
# 填入 SSID 和密码
```

或直接修改 `sdkconfig.defaults`：

```
CONFIG_WIFI_SSID="YourWiFiSSID"
CONFIG_WIFI_PASSWORD="YourWiFiPassword"
```

### 4. 编译、烧录、监控

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## API 调用说明

### 请求格式（OpenAI 兼容）

```
POST https://ark.cn-beijing.volces.com/api/v3/chat/completions
Authorization: Bearer your-real-api-key
Content-Type: application/json

{
  "model": "doubao-1-5-pro-32k-250115",
  "messages": [
    { "role": "user", "content": "你好！" }
  ]
}
```

### 响应解析

自动从响应 JSON 中提取 `choices[0].message.content`：

```c
doubao_result_t result = {0};
esp_err_t err = doubao_chat("你好，请介绍一下自己", &result);
if (err == ESP_OK && result.success) {
    ESP_LOGI(TAG, "AI 回复：%s", result.content);
    free(result.content);  // 使用完毕后释放
}
```

### 内存说明

| 缓冲区 | 大小 | 分配位置 |
|--------|------|----------|
| HTTP 接收缓冲区 | 128 KB | PSRAM（`MALLOC_CAP_SPIRAM`） |
| cJSON 解析 | 动态 | 内部 SRAM |
| 任务栈 | 8 KB | 内部 SRAM |

## 注意事项

1. **API Key 安全**：生产环境请勿将 API Key 硬编码在源码中，建议存储在 NVS 加密分区中。
2. **PSRAM 启用**：`sdkconfig.defaults` 已预置 PSRAM 配置，确保编译时 `CONFIG_SPIRAM=y`。
3. **证书**：使用 `esp_crt_bundle_attach` 进行服务器证书验证，无需手动下载证书文件。
4. **模型 ID**：`DOUBAO_MODEL_ID` 需与火山引擎控制台创建的推理接入点 ID 一致。

## 许可证

MIT License
