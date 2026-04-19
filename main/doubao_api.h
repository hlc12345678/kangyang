/**
 * @file doubao_api.h
 * @brief 豆包（火山引擎）大模型 REST API 调用接口声明
 *
 * 本模块封装了通过 esp_http_client 向豆包大模型发送 HTTP POST 请求、
 * 并使用 cJSON 解析返回结果的完整流程，适配 ESP32-S3 (N16R8) 硬件平台。
 *
 * 硬件平台：ESP32-S3 N16R8（16MB Flash + 8MB PSRAM）
 * 开发框架：ESP-IDF v5.5.2
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* -----------------------------------------------------------------------
 * 用户配置区：请根据实际情况修改以下宏定义
 * ----------------------------------------------------------------------- */

/** 豆包 API 的 Endpoint（OpenAI 兼容格式）*/
#define DOUBAO_API_ENDPOINT  "https://ark.cn-beijing.volces.com/api/v3/chat/completions"

/**
 * 火山引擎 API Key（Bearer 令牌）
 * 警告：生产环境中请勿将密钥硬编码在源码中，应通过 NVS/环境变量等方式注入。
 * 使用前请将下方字符串替换为真实的 API Key，否则运行时将返回认证错误。
 */
#define DOUBAO_API_KEY       "your-real-api-key"

/** 占位符 API Key 字符串，用于运行时检测未替换的情况 */
#define DOUBAO_API_KEY_PLACEHOLDER "your-real-api-key"

/** 豆包模型 ID（在火山引擎控制台创建推理接入点后获得）*/
#define DOUBAO_MODEL_ID      "doubao-seed-2-0-pro-260215"

/**
 * HTTP 响应接收缓冲区大小（字节）
 * 大模型回复内容可能较长，使用 128 KB 缓冲区并分配在 PSRAM 上，
 * 避免占用有限的内部 SRAM，防止 OOM 崩溃。
 */
#define DOUBAO_RECV_BUF_SIZE (128 * 1024)

/** HTTP 连接超时（毫秒）*/
#define DOUBAO_HTTP_TIMEOUT_MS 30000

/* -----------------------------------------------------------------------
 * 数据结构
 * ----------------------------------------------------------------------- */

/**
 * @brief API 调用结果结构体
 */
typedef struct {
    char   *content;    /*!< 模型返回的文本内容（choices[0].message.content），
                             由调用方负责使用 free() 释放 */
    int     http_status; /*!< HTTP 响应状态码，200 表示成功 */
    bool    success;    /*!< 调用是否成功 */
} doubao_result_t;

/* -----------------------------------------------------------------------
 * 公共函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 向豆包大模型发送单条用户消息并获取回复
 *
 * 该函数会：
 *  1. 使用 cJSON 构建符合 OpenAI 格式的 JSON 请求体；
 *  2. 通过 esp_http_client 发送 HTTPS POST 请求；
 *  3. 将响应数据写入分配在 PSRAM 的缓冲区；
 *  4. 使用 cJSON 解析响应，提取 choices[0].message.content；
 *  5. 将提取到的文本以堆内存字符串形式返回。
 *
 * @param[in]  user_message  用户发送给模型的文本（UTF-8）
 * @param[out] result        调用结果，内部的 content 字段需调用方 free()
 * @return  esp_err_t  ESP_OK 表示 HTTP 请求及 JSON 解析均成功
 */
esp_err_t doubao_chat(const char *user_message, doubao_result_t *result);

/**
 * @brief 豆包 AI 对话 FreeRTOS 任务入口函数
 *
 * 该任务会循环调用 doubao_chat() 处理来自队列的用户消息。
 * 通过 xTaskCreateWithCaps() 将任务栈分配在 PSRAM 上，
 * 以节省内部 SRAM 资源。
 *
 * @param[in] pvParameters  预留参数，当前未使用（传 NULL 即可）
 */
void doubao_task(void *pvParameters);

/**
 * @brief 启动豆包 AI 对话任务
 *
 * 在 app_main() 中调用，创建并启动 doubao_task。
 *
 * @return  esp_err_t  ESP_OK 表示任务创建成功
 */
esp_err_t doubao_task_start(void);

#ifdef __cplusplus
}
#endif
