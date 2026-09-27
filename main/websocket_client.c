#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_websocket_client.h"

#include "ds18b20.h"
#include "turbidity.h"
#include "websocket_client.h"

#define WEBSOCKET_URL \
     "ws://192.168.0.113:8000/ws/device/esp_p4_001/?role=producer&key=a4ef63f7-697f-4e65-8fa5-0b7b13fe7a5e"

#define TEMPERATURE_SEND_INTERVAL_MS 5000

static const char *TAG = "AQUANEXIS_WS";

static esp_websocket_client_handle_t websocket_client = NULL;
static volatile bool websocket_connected = false;

static void websocket_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    esp_websocket_event_data_t *event =
        (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            websocket_connected = true;
            ESP_LOGI(TAG, "WebSocket connected to the server");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            websocket_connected = false;
            ESP_LOGW(TAG, "WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_ERROR:
            websocket_connected = false;
            ESP_LOGE(TAG, "WebSocket connection error");
            break;

        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(
                TAG,
                "WebSocket message received: %.*s",
                event->data_len,
                event->data_ptr
            );
            break;

        default:
            break;
    }
}

static void websocket_temperature_task(void *arg)
{
    float temperature;
    turbidity_sample_t turbidity;

    while (true) {
        if (websocket_connected) {
            esp_err_t result =
                ds18b20_read_temperature(&temperature);

            esp_err_t turbidity_result = turbidity_read(&turbidity);

            if (result == ESP_OK && turbidity_result == ESP_OK) {
                websocket_client_send_turbidity(temperature, &turbidity);
            } else {
                ESP_LOGE(
                    TAG,
                    "Sensor read failed: temperature=%s turbidity=%s",
                    esp_err_to_name(result),
                    esp_err_to_name(turbidity_result)
                );
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TEMPERATURE_SEND_INTERVAL_MS));
    }
}

void websocket_client_start(void)
{
    if (websocket_client != NULL) {
        ESP_LOGW(TAG, "WebSocket client is already initialized");
        return;
    }

    esp_err_t sensor_result = ds18b20_init();

    if (sensor_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DS18B20 not found on GPIO4: %s",
            esp_err_to_name(sensor_result)
        );
    }

    sensor_result = turbidity_init();
    if (sensor_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Turbidity sensor ADC initialization failed: %s",
            esp_err_to_name(sensor_result)
        );
    }

    esp_websocket_client_config_t config = {
        .uri = WEBSOCKET_URL,
        .disable_auto_reconnect = false,
        .network_timeout_ms = 10000,
    };

    websocket_client = esp_websocket_client_init(&config);

    if (websocket_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    esp_err_t result =
        esp_websocket_register_events(
            websocket_client,
            WEBSOCKET_EVENT_ANY,
            websocket_event_handler,
            NULL
        );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register WebSocket events: %s",
            esp_err_to_name(result)
        );
        esp_websocket_client_destroy(websocket_client);
        websocket_client = NULL;
        return;
    }

    result = esp_websocket_client_start(websocket_client);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start WebSocket client: %s",
            esp_err_to_name(result)
        );
        esp_websocket_client_destroy(websocket_client);
        websocket_client = NULL;
        return;
    }

    xTaskCreate(
        websocket_temperature_task,
        "websocket_temperature",
        4096,
        NULL,
        5,
        NULL
    );

    ESP_LOGI(TAG, "WebSocket client started");
}

esp_err_t websocket_client_send_temperature(float temperature)
{
    if (websocket_client == NULL || !websocket_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[64];
    int payload_length =
        snprintf(
            payload,
            sizeof(payload),
            "{\"temperature\":%.2f}",
            temperature
        );

    if (payload_length < 0 ||
        payload_length >= (int)sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int sent =
        esp_websocket_client_send_text(
            websocket_client,
            payload,
            payload_length,
            pdMS_TO_TICKS(5000)
        );

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send temperature");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Temperature sent: %.2f C", temperature);
    return ESP_OK;
}

esp_err_t websocket_client_send_turbidity(
    float temperature,
    const turbidity_sample_t *sample
)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (websocket_client == NULL || !websocket_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[128];
    int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{\"temperature\":%.2f,\"turbidity_raw\":%d,\"turbidity_voltage\":%.3f}",
        temperature,
        sample->raw,
        sample->sensor_voltage
    );

    if (payload_length < 0 || payload_length >= (int)sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int sent = esp_websocket_client_send_text(
        websocket_client,
        payload,
        payload_length,
        pdMS_TO_TICKS(5000)
    );

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send temperature and turbidity");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Sensors sent: %.2f C, turbidity AO %.3f V (raw %d)",
        temperature,
        sample->sensor_voltage,
        sample->raw
    );
    return ESP_OK;
}
