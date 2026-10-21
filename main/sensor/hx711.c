#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hx711.h"

#define HX711_DOUT_GPIO GPIO_NUM_6
#define HX711_SCK_GPIO GPIO_NUM_5
#define HX711_CALIBRATION_FACTOR 928.77f
#define HX711_READY_TIMEOUT_MS 2000
#define HX711_GAIN_PULSES 1

static const char *TAG = "HX711";

static int32_t tare_offset;
static bool initialized;

static esp_err_t hx711_wait_ready(void)
{
    TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(HX711_READY_TIMEOUT_MS);

    while (gpio_get_level(HX711_DOUT_GPIO) != 0) {
        if (xTaskGetTickCount() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

static esp_err_t hx711_read_raw(int32_t *raw)
{
    if (!initialized || raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = hx711_wait_ready();
    if (result != ESP_OK) {
        return result;
    }

    uint32_t value = 0;
    for (int bit = 0; bit < 24; bit++) {
        gpio_set_level(HX711_SCK_GPIO, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(HX711_DOUT_GPIO);
        gpio_set_level(HX711_SCK_GPIO, 0);
        esp_rom_delay_us(1);
    }

    for (int pulse = 0; pulse < HX711_GAIN_PULSES; pulse++) {
        gpio_set_level(HX711_SCK_GPIO, 1);
        esp_rom_delay_us(1);
        gpio_set_level(HX711_SCK_GPIO, 0);
        esp_rom_delay_us(1);
    }

    if ((value & 0x800000U) != 0) {
        value |= 0xFF000000U;
    }

    *raw = (int32_t)value;
    return ESP_OK;
}

esp_err_t hx711_init(void)
{
    gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << HX711_DOUT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&input_config);
    if (result != ESP_OK) {
        return result;
    }

    gpio_config_t clock_config = {
        .pin_bit_mask = 1ULL << HX711_SCK_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    result = gpio_config(&clock_config);
    if (result != ESP_OK) {
        return result;
    }

    gpio_set_level(HX711_SCK_GPIO, 0);
    initialized = true;
    tare_offset = 0;

    ESP_LOGI(TAG, "HX711 initialized (DOUT GPIO%d, SCK GPIO%d)",
             HX711_DOUT_GPIO, HX711_SCK_GPIO);
    return ESP_OK;
}

esp_err_t hx711_tare(uint8_t samples)
{
    if (!initialized || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t total = 0;
    for (uint8_t sample = 0; sample < samples; sample++) {
        int32_t raw;
        esp_err_t result = hx711_read_raw(&raw);
        if (result != ESP_OK) {
            return result;
        }
        total += raw;
    }

    tare_offset = (int32_t)(total / samples);
    ESP_LOGI(TAG, "Tare complete: offset=%ld (%u samples)",
             (long)tare_offset, samples);
    return ESP_OK;
}

esp_err_t hx711_read_weight(float *weight_grams, uint8_t samples)
{
    if (weight_grams == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t total = 0;
    for (uint8_t sample = 0; sample < samples; sample++) {
        int32_t raw;
        esp_err_t result = hx711_read_raw(&raw);
        if (result != ESP_OK) {
            return result;
        }
        total += raw;
    }

    float average_raw = (float)total / samples;
    *weight_grams = (average_raw - tare_offset) / HX711_CALIBRATION_FACTOR;
    return ESP_OK;
}