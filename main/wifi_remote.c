// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include "sdkconfig.h"

static bool initialized = false;

bool wifi_remote_get_initialized(void) {
    return initialized;
}

#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL) || \
    defined(CONFIG_BSP_TARGET_HACKERHOTEL_2026)
/// Implementation for Tanmatsu

#include <string.h>
#include "bsp/power.h"
#include "esp_err.h"
#include "esp_hosted_custom.h"
#include "esp_log.h"
#include "host/port/sdio_wrapper.h"
#include "sdkconfig.h"

static const char* TAG = "WiFi remote";

static esp_err_t wifi_remote_verify_radio_ready(void) {
    void* card = hosted_sdio_init();
    if (card == NULL) {
        ESP_LOGE(TAG, "Failed to initialize SDIO for radio");
        return ESP_FAIL;
    }
    esp_err_t res = hosted_sdio_card_init(NULL);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Radio ready");
    } else {
        ESP_LOGI(TAG, "Radio not ready");
    }
    return res;
}

esp_err_t wifi_remote_initialize(void) {
    if (initialized) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Switching radio off...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGW(TAG, "Switching radio to application mode...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "Testing connection to radio...\r\n");
    if (wifi_remote_verify_radio_ready() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Starting ESP hosted...\r\n");
    esp_hosted_host_init();
    initialized = true;
    return ESP_OK;
}

#elif defined(CONFIG_IDF_TARGET_ESP32P4)
/// Generic implementation for ESP32-P4 targets

#include "esp_err.h"
#include "esp_hosted_custom.h"

esp_err_t wifi_remote_initialize(void) {
    if (initialized) {
        return ESP_OK;
    }
    esp_hosted_host_init();
    initialized = true;
    return ESP_OK;
}

#else
/// Generic stub implementation for all devices using an ESP32 with built-in WiFi radio

#include "esp_err.h"

esp_err_t wifi_remote_initialize(void) {
    if (initialized) {
        return ESP_OK;
    }
    initialized = true;
    return ESP_OK;
}

#endif
