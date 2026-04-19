/**
 * @file main.c
 * @brief 智能药盒主程序入口
 *
 * 负责：
 *  1. 初始化 NVS（Non-Volatile Storage）
 *  2. 初始化 Wi-Fi 并等待连接成功
 *  3. 启动豆包 AI 对话 FreeRTOS 任务
 *
 * 硬件平台：ESP32-S3 N16R8（16MB Flash + 8MB PSRAM）
 * 开发框架：ESP-IDF v5.5.2
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "doubao_api.h"

/* 日志 TAG */
static const char *TAG = "kangyang_main";

/* Wi-Fi 连接配置：请修改为实际 Wi-Fi 名称和密码 */
#define WIFI_SSID      CONFIG_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_WIFI_PASSWORD

/** Wi-Fi 事件组，用于在任务间同步连接状态 */
static EventGroupHandle_t s_wifi_event_group;

/** 事件组 bit：IP 地址已获取（Wi-Fi 已成功连接） */
#define WIFI_CONNECTED_BIT BIT0
/** 事件组 bit：Wi-Fi 连接失败 */
#define WIFI_FAIL_BIT      BIT1

/** 最大 Wi-Fi 重连次数 */
#define WIFI_MAX_RETRY     5

/** 当前已重试次数 */
static int s_retry_num = 0;

/* -----------------------------------------------------------------------
 * Wi-Fi 事件处理
 * ----------------------------------------------------------------------- */

/**
 * @brief Wi-Fi 和 IP 事件回调
 *
 * 处理以下事件：
 *  - WIFI_EVENT_STA_START：Wi-Fi Station 启动，触发连接
 *  - WIFI_EVENT_STA_DISCONNECTED：连接断开，自动重连（最多 WIFI_MAX_RETRY 次）
 *  - IP_EVENT_STA_GOT_IP：成功获取 IP 地址，置位 WIFI_CONNECTED_BIT
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Station 启动，开始连接 */
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi Station 已启动，正在尝试连接...");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi 连接断开，正在第 %d/%d 次重连...",
                     s_retry_num, WIFI_MAX_RETRY);
        } else {
            /* 超过最大重试次数，标记失败 */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi 连接失败，已达最大重试次数 %d", WIFI_MAX_RETRY);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "已获取 IP 地址：" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 初始化 Wi-Fi Station 模式并等待连接成功
 *
 * 使用 ESP-IDF 官方推荐的事件驱动方式初始化 Wi-Fi，
 * 通过事件组同步等待连接结果，阻塞直到成功或失败。
 *
 * @return ESP_OK    成功连接并获取 IP
 *         ESP_FAIL  连接失败
 */
static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* 初始化底层 TCP/IP 协议栈 */
    ESP_ERROR_CHECK(esp_netif_init());
    /* 创建默认事件循环 */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* 创建默认 Wi-Fi Station 网络接口 */
    esp_netif_create_default_wifi_sta();

    /* 使用默认配置初始化 Wi-Fi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册 Wi-Fi 和 IP 事件处理器 */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    /* 配置 Wi-Fi SSID 和密码 */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* WPA3 个人模式阈值（兼容大多数路由器） */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi 初始化完成，正在等待连接...");

    /* 阻塞等待连接成功或失败 */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,    /* 不清除 bit，保留状态 */
        pdFALSE,    /* 任一 bit 置位即返回 */
        portMAX_DELAY
    );

    /* 判断连接结果 */
    esp_err_t result;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi 已成功连接到 SSID: %s", WIFI_SSID);
        result = ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi 连接 SSID: %s 失败", WIFI_SSID);
        result = ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "未预期的事件组状态");
        result = ESP_FAIL;
    }

    /* 注销事件处理器（连接建立后不再需要） */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    return result;
}

/* -----------------------------------------------------------------------
 * 应用主入口
 * ----------------------------------------------------------------------- */

/**
 * @brief ESP-IDF 应用主函数
 *
 * 程序启动后，ESP-IDF 框架会在一个独立任务（main task）中调用此函数。
 * 此函数完成系统初始化后，启动豆包 AI 对话任务，然后返回（main task 退出）。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== 智能药盒系统启动 ===");
    ESP_LOGI(TAG, "芯片：ESP32-S3 | Flash：16MB | PSRAM：8MB");
    ESP_LOGI(TAG, "固件框架：ESP-IDF v5.5.2");

    /* ------------------------------------------------------------------
     * 1. 初始化 NVS（非易失性存储）
     *    Wi-Fi 驱动需要 NVS 存储校准数据和配置
     * ------------------------------------------------------------------ */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区损坏或版本不匹配，擦除后重新初始化 */
        ESP_LOGW(TAG, "NVS 需要擦除重建: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS 初始化完成");

    /* ------------------------------------------------------------------
     * 2. 连接 Wi-Fi
     * ------------------------------------------------------------------ */
    err = wifi_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 连接失败，无法调用豆包 API，系统停止");
        /* 在实际产品中可以在此进入配网模式或错误状态 */
        return;
    }

    /* ------------------------------------------------------------------
     * 3. 启动豆包 AI 对话 FreeRTOS 任务
     * ------------------------------------------------------------------ */
    err = doubao_task_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "豆包任务启动失败");
        return;
    }

    ESP_LOGI(TAG, "系统初始化完成，豆包 AI 任务已启动");
    /* app_main 返回后，main task 自动退出，其他任务继续运行 */
}
