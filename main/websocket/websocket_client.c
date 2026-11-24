#include <stdio.h>
#include <stdbool.h>
#include <float.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "nvs.h"

#include "camera_stream.h"
#include "ds18b20.h"
#include "hx711.h"
#include "mg996r.h"
#include "turbidity.h"
#include "websocket_client.h"

#define SENSOR_SEND_INTERVAL_MS 1000
#define HX711_TARE_SAMPLES 20
#define HX711_WEIGHT_SAMPLES 10
#define WEBSOCKET_URL_MAX_LENGTH 255
#define WEBSOCKET_MESSAGE_MAX_LENGTH 512
#define WEBSOCKET_NVS_NAMESPACE "ws_config"
#define WEBSOCKET_NVS_KEY "device_ws_url"

typedef struct {
    int angle;
    float weight_grams;
} servo_control_command_t;

static const char *TAG = "AQUANEXIS_WS";

static esp_websocket_client_handle_t websocket_client = NULL;
static volatile bool websocket_connected = false;
static char websocket_url[WEBSOCKET_URL_MAX_LENGTH + 1];
static char active_websocket_url[WEBSOCKET_URL_MAX_LENGTH + 1];
static TaskHandle_t temperature_task_handle = NULL;
static QueueHandle_t servo_command_queue = NULL;
static bool hx711_available = false;
static char websocket_message_buffer[WEBSOCKET_MESSAGE_MAX_LENGTH + 1];
static size_t websocket_message_length;
static size_t websocket_message_received;

static void reset_websocket_message(void)
{
    websocket_message_length = 0;
    websocket_message_received = 0;
}

static const char *json_get_field_value(const char *json, const char *key)
{
    size_t key_length = strlen(key);
    size_t depth = 0;
    const char *cursor = json;

    while (*cursor != '\0') {
        if (*cursor == '"') {
            const char *string_start = cursor + 1;
            const char *string_end = string_start;
            while (*string_end != '\0') {
                if (*string_end == '\\') {
                    if (string_end[1] == '\0') {
                        return NULL;
                    }
                    string_end += 2;
                } else if (*string_end == '"') {
                    break;
                } else {
                    string_end++;
                }
            }
            if (*string_end != '"') {
                return NULL;
            }

            const char *after_string = string_end + 1;
            while (*after_string == ' ' || *after_string == '\t' ||
                   *after_string == '\r' || *after_string == '\n') {
                after_string++;
            }
            if (depth == 1 &&
                (size_t)(string_end - string_start) == key_length &&
                memcmp(string_start, key, key_length) == 0 &&
                *after_string == ':') {
                after_string++;
                while (*after_string == ' ' || *after_string == '\t' ||
                       *after_string == '\r' || *after_string == '\n') {
                    after_string++;
                }
                return after_string;
            }

            cursor = string_end + 1;
            continue;
        }

        if (*cursor == '{' || *cursor == '[') {
            depth++;
        } else if (*cursor == '}' || *cursor == ']') {
            if (depth == 0) {
                return NULL;
            }
            depth--;
        }
        cursor++;
    }

    return NULL;
}

static bool json_get_string_field(
    const char *json,
    const char *key,
    char *value,
    size_t value_size)
{
    const char *cursor = json_get_field_value(json, key);
    if (cursor == NULL || *cursor != '"' || value_size == 0) {
        return false;
    }

    cursor++;
    size_t length = 0;
    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\' || length + 1 >= value_size) {
            return false;
        }
        value[length++] = *cursor++;
    }
    if (*cursor != '"') {
        return false;
    }

    value[length] = '\0';
    return true;
}

static bool json_get_number_field(
    const char *json,
    const char *key,
    double *value)
{
    const char *number_start = json_get_field_value(json, key);
    if (number_start == NULL) {
        return false;
    }

    const char *cursor = number_start;
    if (*cursor == '-') {
        cursor++;
    }
    if (*cursor == '0') {
        cursor++;
    } else {
        if (*cursor < '1' || *cursor > '9') {
            return false;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
    }
    if (*cursor == '.') {
        cursor++;
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
    }
    if (*cursor == 'e' || *cursor == 'E') {
        cursor++;
        if (*cursor == '+' || *cursor == '-') {
            cursor++;
        }
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
    }

    const char *number_end = cursor;
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    if (*cursor != ',' && *cursor != '}') {
        return false;
    }

    char *conversion_end;
    double parsed_value = strtod(number_start, &conversion_end);
    if (conversion_end != number_end || !isfinite(parsed_value)) {
        return false;
    }

    *value = parsed_value;
    return true;
}

static void handle_servo_control_message(const char *message)
{
    char type[32];
    if (!json_get_string_field(message, "type", type, sizeof(type))) {
        ESP_LOGW(TAG, "Ignoring invalid WebSocket JSON command");
        return;
    }
    if (strcmp(type, "servo_control") != 0) {
        return;
    }

    char servo_id[32];
    double angle;
    double weight_grams;
    if (!json_get_string_field(
            message, "servo_id", servo_id, sizeof(servo_id)) ||
        strcmp(servo_id, "feeder") != 0) {
        ESP_LOGW(TAG, "Ignoring servo command for unsupported servo_id");
        return;
    }

    if (!json_get_number_field(message, "angle", &angle) ||
        angle < 0 ||
        angle > 180 ||
        angle != (double)(int)angle ||
        !json_get_number_field(message, "weight_grams", &weight_grams) ||
        weight_grams <= 0 ||
        weight_grams > (double)FLT_MAX) {
        ESP_LOGW(TAG, "Ignoring servo command with invalid angle or weight_grams");
        return;
    }

    if (servo_command_queue == NULL) {
        ESP_LOGE(TAG, "Cannot queue servo command: command queue is unavailable");
        return;
    }

    servo_control_command_t command = {
        .angle = (int)angle,
        .weight_grams = (float)weight_grams,
    };
    if (xQueueOverwrite(servo_command_queue, &command) != pdPASS) {
        ESP_LOGE(TAG, "Failed to queue servo command");
    }
}

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
            if (event->data_ptr == NULL ||
                event->data_len < 0 ||
                event->payload_len <= 0 ||
                event->payload_len > WEBSOCKET_MESSAGE_MAX_LENGTH) {
                reset_websocket_message();
                break;
            }

            if (event->payload_offset == 0) {
                reset_websocket_message();
                if (event->op_code != 0x1) {
                    break;
                }
                websocket_message_length = (size_t)event->payload_len;
            } else if (websocket_message_length == 0 ||
                       (event->op_code != 0x0 && event->op_code != 0x1)) {
                reset_websocket_message();
                break;
            }

            if (websocket_message_length == 0 ||
                event->payload_len != (int)websocket_message_length ||
                event->payload_offset != (int)websocket_message_received ||
                (size_t)event->data_len >
                    websocket_message_length - websocket_message_received) {
                reset_websocket_message();
                ESP_LOGW(TAG, "Ignoring incomplete or invalid WebSocket message");
                break;
            }

            memcpy(
                websocket_message_buffer + websocket_message_received,
                event->data_ptr,
                (size_t)event->data_len
            );
            websocket_message_received += (size_t)event->data_len;

            if (websocket_message_received == websocket_message_length) {
                websocket_message_buffer[websocket_message_length] = '\0';
                handle_servo_control_message(websocket_message_buffer);
                reset_websocket_message();
            }
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
    bool servo_control_active = false;
    float servo_baseline_weight = 0;
    float servo_weight_threshold = 0;

    while (true) {
        servo_control_command_t command;
        bool have_command =
            servo_command_queue != NULL &&
            xQueueReceive(servo_command_queue, &command, 0) == pdPASS;

        esp_err_t weight_result = ESP_ERR_INVALID_STATE;
        if (hx711_available &&
            (websocket_connected || servo_control_active || have_command)) {
            weight_result =
                hx711_read_weight(&weight_grams, HX711_WEIGHT_SAMPLES);
        }

        if (have_command) {
            if (weight_result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Cannot apply feeder command without a valid weight reading: %s",
                    esp_err_to_name(weight_result)
                );
            } else {
                esp_err_t servo_result = mg996r_set_angle(command.angle);
                if (servo_result != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "Failed to set feeder servo angle: %s",
                        esp_err_to_name(servo_result)
                    );
                } else {
                    servo_baseline_weight = weight_grams;
                    servo_weight_threshold = command.weight_grams;
                    servo_control_active = true;
                    ESP_LOGI(
                        TAG,
                        "Feeder servo set to %d degrees; returning to 90 degrees after %.2f g decrease from %.2f g",
                        command.angle,
                        servo_weight_threshold,
                        servo_baseline_weight
                    );
                }
            }
        }

        if (servo_control_active && weight_result == ESP_OK &&
            servo_baseline_weight - weight_grams >= servo_weight_threshold) {
            esp_err_t servo_result = mg996r_set_angle(90);
            if (servo_result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Failed to return feeder servo to 90 degrees: %s",
                    esp_err_to_name(servo_result)
                );
            } else {
                servo_control_active = false;
                ESP_LOGI(
                    TAG,
                    "Weight decreased by %.2f g; feeder servo returned to 90 degrees",
                    servo_baseline_weight - weight_grams
                );
            }
        }

        if (!websocket_connected && servo_control_active &&
            weight_result != ESP_OK) {
            ESP_LOGE(TAG, "HX711 read failed during feeder control: %s",
                     esp_err_to_name(weight_result));
        }

        if (websocket_connected) {
            esp_err_t result =
                ds18b20_read_temperature(&temperature);

            esp_err_t turbidity_result = turbidity_read(&turbidity);

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

    if (servo_command_queue == NULL) {
        servo_command_queue = xQueueCreate(1, sizeof(servo_control_command_t));
        if (servo_command_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create servo command queue");
            return;
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
