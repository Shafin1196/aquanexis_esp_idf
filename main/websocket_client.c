#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "nvs.h"

#include "camera_stream.h"
#include "ds18b20.h"
#include "hx711.h"
#include "turbidity.h"
#include "websocket_client.h"

#define SENSOR_SEND_INTERVAL_MS 1000
#define HX711_TARE_SAMPLES 20
#define HX711_WEIGHT_SAMPLES 10
#define WEBSOCKET_URL_MAX_LENGTH 255
#define WEBSOCKET_NVS_NAMESPACE "ws_config"
#define WEBSOCKET_NVS_KEY "device_ws_url"

static const char *TAG = "AQUANEXIS_WS";

static esp_websocket_client_handle_t websocket_client = NULL;
static volatile bool websocket_connected = false;
static char websocket_url[WEBSOCKET_URL_MAX_LENGTH + 1];
static char active_websocket_url[WEBSOCKET_URL_MAX_LENGTH + 1];
static TaskHandle_t temperature_task_handle = NULL;
static bool hx711_available = false;

static esp_err_t validate_websocket_url(const char *url, size_t *url_length)
{
    if (url == NULL ||
        (strncmp(url, "ws://", 5) != 0 &&
         strncmp(url, "wss://", 6) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strlen(url);
    if (length == 0 || length > WEBSOCKET_URL_MAX_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *host =
        strncmp(url, "wss://", 6) == 0 ? url + 6 : url + 5;
    if (*host == '\0' || *host == '/') {
        return ESP_ERR_INVALID_ARG;
    }

    if (url_length != NULL) {
        *url_length = length;
    }

    return ESP_OK;
}

esp_err_t websocket_client_load_url(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t result =
        nvs_open(WEBSOCKET_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved WebSocket URL; waiting for BLE provisioning");
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WebSocket settings: %s",
                 esp_err_to_name(result));
        return result;
    }

    size_t url_size = sizeof(websocket_url);
    result = nvs_get_str(nvs_handle, WEBSOCKET_NVS_KEY, websocket_url, &url_size);
    nvs_close(nvs_handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved WebSocket URL; waiting for BLE provisioning");
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        websocket_url[0] = '\0';
        ESP_LOGE(TAG, "Failed to read saved WebSocket URL: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = validate_websocket_url(websocket_url, NULL);
    if (result != ESP_OK) {
        websocket_url[0] = '\0';
        ESP_LOGE(TAG, "Saved WebSocket URL is invalid");
        return result;
    }

    ESP_LOGI(TAG, "Restored WebSocket URL from NVS");
    return ESP_OK;
}

esp_err_t websocket_client_set_url(const char *url)
{
    size_t url_length;
    esp_err_t result = validate_websocket_url(url, &url_length);
    if (result != ESP_OK) {
        return result;
    }

    nvs_handle_t nvs_handle;
    result = nvs_open(WEBSOCKET_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WebSocket settings: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = nvs_set_str(nvs_handle, WEBSOCKET_NVS_KEY, url);
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WebSocket URL: %s",
                 esp_err_to_name(result));
        return result;
    }

    memcpy(websocket_url, url, url_length + 1);
    ESP_LOGI(TAG, "Saved WebSocket URL to NVS");
    return ESP_OK;
}

esp_err_t websocket_client_clear_url(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t result =
        nvs_open(WEBSOCKET_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        websocket_url[0] = '\0';
        active_websocket_url[0] = '\0';
        return ESP_OK;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WebSocket settings: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = nvs_erase_key(nvs_handle, WEBSOCKET_NVS_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear saved WebSocket URL: %s",
                 esp_err_to_name(result));
        return result;
    }

    websocket_url[0] = '\0';
    active_websocket_url[0] = '\0';
    ESP_LOGI(TAG, "Cleared saved WebSocket URL");
    return ESP_OK;
}

bool websocket_client_is_connected(void)
{
    return websocket_connected;
}

esp_err_t websocket_client_send_text_payload(const char *payload)
{
    if (websocket_client == NULL || !websocket_connected || payload == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(
        websocket_client,
        payload,
        strlen(payload),
        pdMS_TO_TICKS(5000)
    );

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send text payload");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t websocket_client_send_binary(const uint8_t *data, size_t length)
{
    if (websocket_client == NULL || !websocket_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int sent = esp_websocket_client_send_bin(
        websocket_client,
        (const char *)data,
        (int)length,
        pdMS_TO_TICKS(5000)
    );

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send binary video frame (%d bytes)", (int)length);
        return ESP_FAIL;
    }

    return ESP_OK;
}

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
            camera_stream_start();
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
    float weight_grams;
    turbidity_sample_t turbidity;

    while (true) {
        if (websocket_connected) {
            esp_err_t result =
                ds18b20_read_temperature(&temperature);

            esp_err_t turbidity_result = turbidity_read(&turbidity);
            esp_err_t weight_result = ESP_ERR_INVALID_STATE;
            if (hx711_available) {
                weight_result = hx711_read_weight(
                    &weight_grams,
                    HX711_WEIGHT_SAMPLES
                );
            }

            if (result == ESP_OK && turbidity_result == ESP_OK) {
                websocket_client_send_turbidity(
                    temperature,
                    &turbidity,
                    weight_result == ESP_OK ? &weight_grams : NULL
                );
            } else {
                ESP_LOGE(
                    TAG,
                    "Sensor read failed: temperature=%s turbidity=%s",
                    esp_err_to_name(result),
                    esp_err_to_name(turbidity_result)
                );
            }

            if (weight_result != ESP_OK && hx711_available) {
                ESP_LOGE(
                    TAG,
                    "HX711 read failed: %s",
                    esp_err_to_name(weight_result)
                );
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_SEND_INTERVAL_MS));
    }
}

void websocket_client_start(void)
{
    if (websocket_url[0] == '\0') {
        ESP_LOGE(TAG, "WebSocket URL has not been provided");
        return;
    }

    if (websocket_client != NULL &&
        strcmp(active_websocket_url, websocket_url) == 0) {
        ESP_LOGW(TAG, "WebSocket client is already initialized");
        return;
    }

    if (websocket_client != NULL) {
        esp_err_t stop_result = esp_websocket_client_stop(websocket_client);
        if (stop_result != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to stop previous WebSocket client: %s",
                esp_err_to_name(stop_result)
            );
        }

        esp_websocket_client_destroy(websocket_client);
        websocket_client = NULL;
        websocket_connected = false;
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

    sensor_result = hx711_init();
    if (sensor_result != ESP_OK) {
        hx711_available = false;
        ESP_LOGE(
            TAG,
            "HX711 initialization failed on DOUT GPIO6/SCK GPIO5: %s",
            esp_err_to_name(sensor_result)
        );
    } else {
        sensor_result = hx711_tare(HX711_TARE_SAMPLES);
        hx711_available = sensor_result == ESP_OK;
        if (sensor_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "HX711 tare failed; weight will be omitted: %s",
                esp_err_to_name(sensor_result)
            );
        }
    }

    esp_websocket_client_config_t config = {
        .uri = websocket_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = false,
        .network_timeout_ms = 10000,
    };

    websocket_client = esp_websocket_client_init(&config);

    if (websocket_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }
    memcpy(active_websocket_url, websocket_url, strlen(websocket_url) + 1);

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

    if (temperature_task_handle == NULL) {
        BaseType_t task_result = xTaskCreate(
            websocket_temperature_task,
            "websocket_temperature",
            4096,
            NULL,
            5,
            &temperature_task_handle
        );

        if (task_result != pdPASS) {
            temperature_task_handle = NULL;
            ESP_LOGE(TAG, "Failed to create WebSocket temperature task");
        }
    }

    ESP_LOGI(TAG, "WebSocket client started; waiting for camera stream start");

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
    const turbidity_sample_t *sample,
    const float *weight_grams
)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (websocket_client == NULL || !websocket_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[160];
    int payload_length;
    if (weight_grams != NULL) {
        payload_length = snprintf(
            payload,
            sizeof(payload),
            "{\"temperature\":%.2f,\"turbidity_raw\":%d,\"turbidity_voltage\":%.3f,\"weight_grams\":%.2f}",
            temperature,
            sample->raw,
            sample->sensor_voltage,
            *weight_grams
        );
    } else {
        payload_length = snprintf(
            payload,
            sizeof(payload),
            "{\"temperature\":%.2f,\"turbidity_raw\":%d,\"turbidity_voltage\":%.3f}",
            temperature,
            sample->raw,
            sample->sensor_voltage
        );
    }

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

    if (weight_grams != NULL) {
        ESP_LOGI(
            TAG,
            "Sensors sent: %.2f C, turbidity AO %.3f V (raw %d), weight %.2f g",
            temperature,
            sample->sensor_voltage,
            sample->raw,
            *weight_grams
        );
    } else {
        ESP_LOGI(
            TAG,
            "Sensors sent: %.2f C, turbidity AO %.3f V (raw %d), weight omitted",
            temperature,
            sample->sensor_voltage,
            sample->raw
        );
    }
    return ESP_OK;
}
