#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_station.h"
#include "ble_test.h"
#include "websocket_client.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_CLEAR_BUTTON_GPIO GPIO_NUM_35

static const char *TAG = "AQUANEXIS_WIFI";

static EventGroupHandle_t wifi_event_group = NULL;

static int retry_count = 0;

static bool wifi_clear_button_is_pressed(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << WIFI_CLEAR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    return gpio_get_level(WIFI_CLEAR_BUTTON_GPIO) == 0;
}

static void wifi_clear_button_task(void *arg)
{
    bool handled = false;

    while (true) {
        bool pressed =
            gpio_get_level(WIFI_CLEAR_BUTTON_GPIO) == 0;

        if (pressed && !handled) {
            ESP_LOGW(
                TAG,
                "BOOT button pressed; clearing saved Wi-Fi credentials"
            );

            esp_err_t result = esp_wifi_restore();
            if (result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Failed to clear Wi-Fi credentials: %s",
                    esp_err_to_name(result)
                );
            } else {
                esp_wifi_disconnect();
                ESP_LOGI(
                    TAG,
                    "Wi-Fi credentials cleared; provision again over BLE"
                );
            }

            handled = true;
        } else if (!pressed) {
            handled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/*
 * Wi-Fi event handler
 */
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    /*
     * Wi-Fi station started
     */
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(
            TAG,
            "Wi-Fi station started"
        );

    }


    /*
     * Wi-Fi disconnected
     */
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        if (retry_count < 5) {

            retry_count++;

            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected. Retrying... (%d/5)",
                retry_count
            );

            esp_wifi_connect();

        }

        else {

            ESP_LOGE(
                TAG,
                "Wi-Fi connection failed"
            );

            xEventGroupSetBits(
                wifi_event_group,
                WIFI_FAIL_BIT
            );
        }
    }


    /*
     * Wi-Fi connected and IP received
     */
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "================================"
        );

        ESP_LOGI(
            TAG,
            "Wi-Fi connected!"
        );

        ESP_LOGI(
            TAG,
            "IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        ESP_LOGI(
            TAG,
            "================================"
        );

        retry_count = 0;

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        websocket_client_start();
    }
}


/*
 * Initialize Wi-Fi subsystem.
 *
 * IMPORTANT:
 * This function does NOT connect to Wi-Fi.
 *
 * Credentials will later come from BLE.
 */
void wifi_station_start(void)
{
    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create Wi-Fi event group"
        );

        return;
    }


    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    esp_netif_create_default_wifi_sta();


    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();


    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    if (wifi_clear_button_is_pressed()) {
        ESP_LOGW(
            TAG,
            "BOOT button held; clearing saved Wi-Fi credentials"
        );

        ESP_ERROR_CHECK(
            esp_wifi_restore()
        );
    }


    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );


    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );


    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    wifi_config_t saved_config = {0};
    esp_err_t saved_config_result =
        esp_wifi_get_config(WIFI_IF_STA, &saved_config);

    if (saved_config_result == ESP_OK &&
        saved_config.sta.ssid[0] != '\0') {
        ESP_LOGI(
            TAG,
            "Saved Wi-Fi credentials found for SSID: %s",
            (char *)saved_config.sta.ssid
        );

        retry_count = 0;
        ESP_ERROR_CHECK(
            esp_wifi_connect()
        );
    } else {
        ESP_LOGI(
            TAG,
            "No saved Wi-Fi credentials found; waiting for BLE provisioning"
        );
    }

    xTaskCreate(
        wifi_clear_button_task,
        "wifi_clear_button",
        3072,
        NULL,
        5,
        NULL
    );


    ESP_LOGI(
        TAG,
        "Wi-Fi station initialized"
    );
}


/*
 * Connect to Wi-Fi using credentials
 * received from BLE.
 */
esp_err_t wifi_station_connect(
    const char *ssid,
    const char *password)
{
    if (ssid == NULL ||
        password == NULL) {

        ESP_LOGE(
            TAG,
            "SSID or password is NULL"
        );

        return ESP_ERR_INVALID_ARG;
    }


    if (strlen(ssid) == 0) {

        ESP_LOGE(
            TAG,
            "SSID is empty"
        );

        return ESP_ERR_INVALID_ARG;
    }


    ESP_LOGI(
        TAG,
        "Configuring Wi-Fi..."
    );

    ESP_LOGI(
        TAG,
        "SSID: %s",
        ssid
    );


    wifi_config_t wifi_config = {0};


    strncpy(
        (char *)wifi_config.sta.ssid,
        ssid,
        sizeof(wifi_config.sta.ssid) - 1
    );


    strncpy(
        (char *)wifi_config.sta.password,
        password,
        sizeof(wifi_config.sta.password) - 1
    );


    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;


    retry_count = 0;


    xEventGroupClearBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT |
        WIFI_FAIL_BIT
    );


    /*
     * Stop any previous connection.
     */
    esp_wifi_disconnect();


    /*
     * Apply new credentials.
     */
    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );


    /*
     * Start connection.
     */
    ESP_ERROR_CHECK(
        esp_wifi_connect()
    );


    ESP_LOGI(
        TAG,
        "Connecting to Wi-Fi: %s",
        ssid
    );


    return ESP_OK;
}


/*
 * Wait for Wi-Fi result.
 */
EventBits_t wifi_station_wait_for_result(
    TickType_t timeout)
{
    if (wifi_event_group == NULL) {
        return 0;
    }

    return xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT |
        WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        timeout
    );
}


/*
 * Check whether Wi-Fi is connected.
 */
bool wifi_station_is_connected(void)
{
    if (wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits =
        xEventGroupGetBits(wifi_event_group);

    return (
        bits & WIFI_CONNECTED_BIT
    ) != 0;
}


/*
 * Application entry point.
 */
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


    /*
     * Initialize NVS.
     */
    esp_err_t ret = nvs_flash_init();


    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ret = nvs_flash_init();
    }


    ESP_ERROR_CHECK(ret);


    /*
     * Start Wi-Fi subsystem.
     *
     * Do NOT connect yet.
     * Credentials will come from BLE.
     */
    wifi_station_start();


    /*
     * Start BLE provisioning.
     */
    ble_test_start();


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


    /*
     * Main task stays alive.
     *
     * BLE and Wi-Fi event handlers work
     * asynchronously in their own tasks.
     */
    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}