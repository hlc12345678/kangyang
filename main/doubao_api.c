/**
 * @file doubao_api.c
 * @brief 豆包（火山引擎）大模型 REST API 实现
 *
 * 功能说明：
 *  - 使用 esp_http_client 向豆包 API 发送 HTTPS POST 请求
 *  - 使用 cJSON 构建 OpenAI 兼容格式的 JSON 请求体
 *  - 使用 cJSON 解析响应，提取 choices[0].message.content
 *  - 接收缓冲区通过 heap_caps_malloc() 分配在 PSRAM（外部存储器），
 *    避免处理长回复时耗尽内部 SRAM 导致崩溃
 *  - 通过 esp_crt_bundle_attach 完成 HTTPS 证书校验
 *  - 整个调用流程运行在独立的 FreeRTOS 任务中
 *
 * 硬件平台：ESP32-S3 N16R8（16MB Flash + 8MB PSRAM）
 * 开发框架：ESP-IDF v5.5.2
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "cJSON.h"

#include "doubao_api.h"

/* 日志 TAG，方便在串口监视器中过滤本模块的输出 */
static const char *TAG = "doubao_api";

/**
 * @brief HTTP 事件处理器内部上下文
 *
 * 将接收缓冲区和已接收字节数打包传递给 HTTP 事件回调函数。
 */
typedef struct {
    char   *recv_buf;       /*!< 接收缓冲区指针（分配在 PSRAM）*/
    size_t  recv_buf_size;  /*!< 缓冲区总容量（字节）*/
    size_t  recv_len;       /*!< 已接收的有效数据长度（字节）*/
} http_event_ctx_t;

/* -----------------------------------------------------------------------
 * 内部辅助函数
 * ----------------------------------------------------------------------- */

/**
 * @brief esp_http_client HTTP 事件回调
 *
 * 每当 HTTP 客户端有新数据到达时，此回调将数据追加到 PSRAM 缓冲区中。
 * 同时处理连接建立、请求完成、断开等事件的日志输出。
 *
 * @param evt  HTTP 事件指针，evt->user_data 指向 http_event_ctx_t
 * @return     ESP_OK（始终返回，内部错误通过日志警告）
 */
static esp_err_t doubao_http_event_handler(esp_http_client_event_t *evt)
{
    http_event_ctx_t *ctx = (http_event_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP 已连接到服务器");
        /* 重置接收长度，准备接收新响应 */
        if (ctx != NULL) {
            ctx->recv_len = 0;
        }
        break;

    case HTTP_EVENT_ON_DATA:
        /* 服务器返回数据片段，追加到接收缓冲区 */
        if (ctx == NULL || ctx->recv_buf == NULL) {
            ESP_LOGW(TAG, "接收缓冲区未初始化，跳过数据片段");
            break;
        }
        if (ctx->recv_len + evt->data_len >= ctx->recv_buf_size) {
            /* 缓冲区空间不足，截断并警告 */
            ESP_LOGW(TAG, "接收缓冲区已满（已收 %u 字节），后续数据将被丢弃",
                     (unsigned)ctx->recv_len);
            break;
        }
        /* 将本次数据片段追加到缓冲区末尾 */
        memcpy(ctx->recv_buf + ctx->recv_len, evt->data, evt->data_len);
        ctx->recv_len += evt->data_len;
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP 响应接收完毕，总计 %u 字节", (unsigned)(ctx ? ctx->recv_len : 0));
        /* 在有效数据末尾补零，使其成为合法 C 字符串 */
        if (ctx != NULL && ctx->recv_buf != NULL) {
            ctx->recv_buf[ctx->recv_len] = '\0';
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP 连接已断开");
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP 事件错误");
        break;

    default:
        break;
    }

    return ESP_OK;
}

/**
 * @brief 使用 cJSON 构建豆包 API 请求体（OpenAI 兼容格式）
 *
 * 构建结构如下的 JSON 字符串：
 * @code{.json}
 * {
 *   "model": "doubao-1-5-pro-32k-250115",
 *   "messages": [
 *     { "role": "user", "content": "<user_message>" }
 *   ]
 * }
 * @endcode
 *
 * @param[in]  user_message  用户输入的文本
 * @return     动态分配的 JSON 字符串（调用方负责 free()），失败返回 NULL
 */
static char *build_request_json(const char *user_message)
{
    char *json_str = NULL;

    /* 创建根 JSON 对象 */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject 失败（根对象）");
        return NULL;
    }

    /* 添加模型 ID 字段 */
    if (!cJSON_AddStringToObject(root, "model", DOUBAO_MODEL_ID)) {
        ESP_LOGE(TAG, "添加 model 字段失败");
        goto cleanup;
    }

    /* 创建 messages 数组 */
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (messages == NULL) {
        ESP_LOGE(TAG, "创建 messages 数组失败");
        goto cleanup;
    }

    /* 创建用户消息对象并加入数组 */
    cJSON *msg = cJSON_CreateObject();
    if (msg == NULL) {
        ESP_LOGE(TAG, "创建 message 对象失败");
        goto cleanup;
    }

    if (!cJSON_AddStringToObject(msg, "role", "user") ||
        !cJSON_AddStringToObject(msg, "content", user_message)) {
        ESP_LOGE(TAG, "添加 role/content 字段失败");
        cJSON_Delete(msg);
        goto cleanup;
    }

    cJSON_AddItemToArray(messages, msg);

    /* 序列化为字符串（非格式化，节省内存） */
    json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted 失败");
    }

cleanup:
    cJSON_Delete(root);
    return json_str;
}

/**
 * @brief 从豆包 API 响应 JSON 中提取 choices[0].message.content
 *
 * 期望解析的 JSON 结构（标准 OpenAI 格式）：
 * @code{.json}
 * {
 *   "choices": [
 *     {
 *       "message": {
 *         "content": "模型的回复文本"
 *       }
 *     }
 *   ]
 * }
 * @endcode
 *
 * @param[in]  json_str    原始响应 JSON 字符串
 * @param[out] out_content 提取到的内容字符串（动态分配，调用方负责 free()）
 * @return  ESP_OK 表示解析成功，ESP_FAIL 表示解析失败
 */
static esp_err_t parse_response_json(const char *json_str, char **out_content)
{
    esp_err_t ret = ESP_FAIL;
    *out_content = NULL;

    /* 解析 JSON */
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON 解析失败，错误位置附近: %s",
                 error_ptr ? error_ptr : "未知");
        return ESP_FAIL;
    }

    /* 获取 choices 数组 */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "响应中缺少 choices 数组或数组为空");
        goto cleanup;
    }

    /* 取 choices[0] */
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if (!cJSON_IsObject(choice0)) {
        ESP_LOGE(TAG, "choices[0] 不是有效的 JSON 对象");
        goto cleanup;
    }

    /* 获取 message 对象 */
    cJSON *message = cJSON_GetObjectItemCaseSensitive(choice0, "message");
    if (!cJSON_IsObject(message)) {
        ESP_LOGE(TAG, "choices[0].message 不是有效的 JSON 对象");
        goto cleanup;
    }

    /* 获取 content 字符串 */
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        ESP_LOGE(TAG, "choices[0].message.content 不是有效的字符串");
        goto cleanup;
    }

    /* 复制到堆内存中返回（调用方负责 free()） */
    *out_content = strdup(content->valuestring);
    if (*out_content == NULL) {
        ESP_LOGE(TAG, "strdup 失败，内存不足");
        goto cleanup;
    }

    ret = ESP_OK;
    ESP_LOGI(TAG, "成功提取模型回复（%u 字节）", (unsigned)strlen(*out_content));

cleanup:
    cJSON_Delete(root);
    return ret;
}

/* -----------------------------------------------------------------------
 * 公共接口实现
 * ----------------------------------------------------------------------- */

/**
 * @brief 向豆包大模型发送单条用户消息并获取回复
 *
 * 详细说明见 doubao_api.h 中的函数声明。
 */
esp_err_t doubao_chat(const char *user_message, doubao_result_t *result)
{
    if (user_message == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 运行时检测：如果 API Key 仍为占位符，提前报错并返回 */
    if (strcmp(DOUBAO_API_KEY, DOUBAO_API_KEY_PLACEHOLDER) == 0) {
        ESP_LOGE(TAG, "API Key 未配置！请在 doubao_api.h 中将 DOUBAO_API_KEY 替换为真实的火山引擎 API Key");
        return ESP_ERR_INVALID_STATE;
    }

    /* 初始化返回结构体 */
    result->content     = NULL;
    result->http_status = 0;
    result->success     = false;

    esp_err_t ret = ESP_FAIL;

    /* ------------------------------------------------------------------
     * 1. 在 PSRAM 上分配接收缓冲区
     *    MALLOC_CAP_SPIRAM：强制分配在外部 PSRAM
     *    MALLOC_CAP_8BIT ：保证字节对齐，适合存储字符串
     * ------------------------------------------------------------------ */
    http_event_ctx_t ctx = {
        .recv_buf      = NULL,
        .recv_buf_size = DOUBAO_RECV_BUF_SIZE,
        .recv_len      = 0,
    };

    ctx.recv_buf = (char *)heap_caps_malloc(
        DOUBAO_RECV_BUF_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ctx.recv_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 缓冲区分配失败（请求 %u 字节），尝试内部 SRAM",
                 (unsigned)DOUBAO_RECV_BUF_SIZE);
        /* 回退到内部 SRAM，但缩小缓冲区以避免 OOM */
        ctx.recv_buf_size = 16 * 1024;
        ctx.recv_buf = (char *)malloc(ctx.recv_buf_size);
        if (ctx.recv_buf == NULL) {
            ESP_LOGE(TAG, "内部 SRAM 缓冲区分配也失败，放弃");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "已回退到内部 SRAM，缓冲区缩小为 %u 字节",
                 (unsigned)ctx.recv_buf_size);
    } else {
        ESP_LOGI(TAG, "PSRAM 接收缓冲区分配成功（%u 字节）",
                 (unsigned)DOUBAO_RECV_BUF_SIZE);
    }

    /* ------------------------------------------------------------------
     * 2. 构建 JSON 请求体
     * ------------------------------------------------------------------ */
    char *request_body = build_request_json(user_message);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "构建请求 JSON 失败");
        ret = ESP_ERR_NO_MEM;
        goto free_buf;
    }
    ESP_LOGD(TAG, "请求体：%s", request_body);

    /* ------------------------------------------------------------------
     * 3. 配置 esp_http_client
     * ------------------------------------------------------------------ */
    esp_http_client_config_t config = {
        .url              = DOUBAO_API_ENDPOINT,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = DOUBAO_HTTP_TIMEOUT_MS,
        .event_handler    = doubao_http_event_handler,
        .user_data        = &ctx,
        /* 使用 ESP-IDF 内置证书包进行 HTTPS 服务器证书校验 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* 允许重定向（豆包 API 可能返回 3xx） */
        .max_redirection_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        ret = ESP_FAIL;
        goto free_json;
    }

    /* ------------------------------------------------------------------
     * 4. 设置请求头
     * ------------------------------------------------------------------ */

    /* Content-Type：通知服务器请求体格式为 JSON */
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 Content-Type 头失败: %s", esp_err_to_name(err));
        goto cleanup_client;
    }

    /* Authorization：Bearer 令牌认证 */
    /* 拼接 "Bearer " + API Key */
    size_t auth_len = strlen("Bearer ") + strlen(DOUBAO_API_KEY) + 1;
    char *auth_value = (char *)malloc(auth_len);
    if (auth_value == NULL) {
        ESP_LOGE(TAG, "Authorization 头内存分配失败");
        ret = ESP_ERR_NO_MEM;
        goto cleanup_client;
    }
    snprintf(auth_value, auth_len, "Bearer %s", DOUBAO_API_KEY);

    err = esp_http_client_set_header(client, "Authorization", auth_value);
    free(auth_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 Authorization 头失败: %s", esp_err_to_name(err));
        goto cleanup_client;
    }

    /* ------------------------------------------------------------------
     * 5. 设置请求体并执行 HTTP 请求
     * ------------------------------------------------------------------ */
    err = esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置请求体失败: %s", esp_err_to_name(err));
        goto cleanup_client;
    }

    ESP_LOGI(TAG, "正在向豆包 API 发送请求...");
    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 请求执行失败: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup_client;
    }

    /* ------------------------------------------------------------------
     * 6. 检查 HTTP 状态码
     * ------------------------------------------------------------------ */
    result->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP 响应状态码: %d", result->http_status);

    if (result->http_status != 200) {
        ESP_LOGE(TAG, "API 返回非 200 状态码，响应体：%s",
                 ctx.recv_len > 0 ? ctx.recv_buf : "(空)");
        ret = ESP_FAIL;
        goto cleanup_client;
    }

    /* ------------------------------------------------------------------
     * 7. 使用 cJSON 解析响应，提取 choices[0].message.content
     * ------------------------------------------------------------------ */
    ESP_LOGD(TAG, "原始响应：%.*s", (int)ctx.recv_len, ctx.recv_buf);

    err = parse_response_json(ctx.recv_buf, &result->content);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "解析响应 JSON 失败");
        ret = ESP_FAIL;
        goto cleanup_client;
    }

    result->success = true;
    ret = ESP_OK;
    ESP_LOGI(TAG, "豆包回复：%s", result->content);

cleanup_client:
    esp_http_client_cleanup(client);

free_json:
    free(request_body);

free_buf:
    /* 释放 PSRAM 接收缓冲区 */
    if (ctx.recv_buf != NULL) {
        heap_caps_free(ctx.recv_buf);
        ctx.recv_buf = NULL;
    }

    return ret;
}

/* -----------------------------------------------------------------------
 * FreeRTOS 任务
 * ----------------------------------------------------------------------- */

/**
 * @brief 豆包 AI 对话 FreeRTOS 任务
 *
 * 任务栈分配在 PSRAM 上（通过 xTaskCreatePinnedToCoreWithCaps），
 * 避免大型任务栈耗尽内部 SRAM。
 * 当前实现为示例性质：发送一条固定消息后进入空闲循环。
 * 实际项目中可通过 FreeRTOS 队列接收来自其他任务的消息。
 */
void doubao_task(void *pvParameters)
{
    ESP_LOGI(TAG, "豆包 AI 对话任务已启动，任务栈大小: %u 字节",
             (unsigned)(CONFIG_DOUBAO_TASK_STACK_SIZE));

    /* 示例：发送一条问题（大模型不能访问实时数据，此处仅作功能演示）*/
    const char *demo_message = "你好！请简单介绍一下自己，以及你能为智能药盒的用户提供哪些帮助？";

    doubao_result_t result = {0};
    esp_err_t err = doubao_chat(demo_message, &result);

    if (err == ESP_OK && result.success) {
        ESP_LOGI(TAG, "=== 豆包 AI 回复 ===");
        ESP_LOGI(TAG, "%s", result.content);
        ESP_LOGI(TAG, "===================");
        /* 释放由 doubao_chat 内部 strdup() 分配的内存 */
        free(result.content);
        result.content = NULL;
    } else {
        ESP_LOGE(TAG, "doubao_chat 失败，HTTP 状态码: %d", result.http_status);
    }

    /* 任务完成后进入低功耗等待，实际项目中可在此处理队列消息 */
    ESP_LOGI(TAG, "豆包任务演示完成，任务挂起");
    vTaskSuspend(NULL);
}

/**
 * @brief 启动豆包 AI 对话任务
 */
esp_err_t doubao_task_start(void)
{
    BaseType_t xReturned;

    /*
     * 使用 xTaskCreate 创建任务。
     * 若 ESP-IDF 版本支持 xTaskCreateWithCaps（ESP-IDF v5.x+），
     * 可改用如下方式将任务栈分配在 PSRAM：
     *
     *   xTaskCreateWithCaps(doubao_task, "doubao_task",
     *                       CONFIG_DOUBAO_TASK_STACK_SIZE, NULL,
     *                       CONFIG_DOUBAO_TASK_PRIORITY,
     *                       NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
     *
     * 注意：ESP-IDF v5.x 的 xTaskCreateWithCaps 需要开启 PSRAM 并在
     * menuconfig 中启用 "Allow external memory for task stacks" 选项。
     */
    xReturned = xTaskCreate(
        doubao_task,                    /* 任务函数 */
        "doubao_task",                  /* 任务名称（用于调试） */
        CONFIG_DOUBAO_TASK_STACK_SIZE,  /* 任务栈大小（字节） */
        NULL,                           /* 任务参数 */
        CONFIG_DOUBAO_TASK_PRIORITY,    /* 任务优先级 */
        NULL                            /* 任务句柄（不需要时传 NULL） */
    );

    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "创建豆包任务失败（可能内存不足）");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "豆包任务已创建，栈大小: %d 字节，优先级: %d",
             CONFIG_DOUBAO_TASK_STACK_SIZE, CONFIG_DOUBAO_TASK_PRIORITY);
    return ESP_OK;
}
