#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_test.h"
#include "mg996r.h"
#include "websocket_client.h"
#include "wifi_station.h"

static const char *TAG = "AQUANEXIS_WIFI";

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        " AquaNexis Wi-Fi Test"
    );

    ESP_LOGI(
        TAG,
        " ESP32-P4 + ESP32-C6"
    );

    ESP_LOGI(
        TAG,
        "================================"
    );

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    esp_err_t websocket_url_result = websocket_client_load_url();
    if (websocket_url_result != ESP_OK &&
        websocket_url_result != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(
            TAG,
            "WebSocket URL restore failed: %s",
            esp_err_to_name(websocket_url_result)
        );
    }

    wifi_station_start();
    esp_err_t servo_result = mg996r_test_start();
    if (servo_result != ESP_OK) {
        ESP_LOGE(TAG, "MG996R servo test failed: %s", esp_err_to_name(servo_result));
    }
#if CONFIG_BT_ENABLED
    ble_test_start();
#else
    ESP_LOGI(TAG, "Local BLE is unavailable on this ESP32-P4 configuration");
#endif

    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "AquaNexis is ready for setup"
    );

    ESP_LOGI(
        TAG,
        "Waiting for Wi-Fi credentials over BLE..."
    );

    ESP_LOGI(
        TAG,
        "================================"
    );

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
