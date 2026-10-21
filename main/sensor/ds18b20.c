#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ds18b20.h"

#define DS18B20_GPIO GPIO_NUM_4

#define DS18B20_CMD_SKIP_ROM       0xCC
#define DS18B20_CMD_CONVERT_T      0x44
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE

#define DS18B20_CONVERSION_TIME_MS 750
#define DS18B20_RESET_WAIT_US      480
#define DS18B20_PRESENCE_WAIT_US   70
#define DS18B20_SLOT_US            60

static const char *TAG = "DS18B20";

static void ds18b20_release_bus(void)
{
    gpio_set_direction(DS18B20_GPIO, GPIO_MODE_INPUT);
}

static void ds18b20_pull_bus_low(uint32_t duration_us)
{
    gpio_set_direction(DS18B20_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DS18B20_GPIO, 0);
    esp_rom_delay_us(duration_us);
}

static bool ds18b20_reset(void)
{
    ds18b20_pull_bus_low(DS18B20_RESET_WAIT_US);
    ds18b20_release_bus();
    esp_rom_delay_us(DS18B20_PRESENCE_WAIT_US);

    bool device_present = gpio_get_level(DS18B20_GPIO) == 0;

    esp_rom_delay_us(DS18B20_RESET_WAIT_US - DS18B20_PRESENCE_WAIT_US);
    return device_present;
}

static void ds18b20_write_bit(uint8_t bit)
{
    ds18b20_pull_bus_low(2);

    if (bit != 0) {
        ds18b20_release_bus();
        esp_rom_delay_us(DS18B20_SLOT_US - 2);
    } else {
        esp_rom_delay_us(DS18B20_SLOT_US - 2);
        ds18b20_release_bus();
    }
}

static uint8_t ds18b20_read_bit(void)
{
    uint8_t bit;

    ds18b20_pull_bus_low(2);
    ds18b20_release_bus();
    esp_rom_delay_us(10);
    bit = (uint8_t)gpio_get_level(DS18B20_GPIO);
    esp_rom_delay_us(DS18B20_SLOT_US - 12);

    return bit;
}

static void ds18b20_write_byte(uint8_t value)
{
    for (int bit = 0; bit < 8; bit++) {
        ds18b20_write_bit(value & (1U << bit));
    }
}

static uint8_t ds18b20_read_byte(void)
{
    uint8_t value = 0;

    for (int bit = 0; bit < 8; bit++) {
        if (ds18b20_read_bit() != 0) {
            value |= (uint8_t)(1U << bit);
        }
    }

    return value;
}

static uint8_t ds18b20_crc8(
    const uint8_t *data,
    size_t length)
{
    uint8_t crc = 0;

    for (size_t index = 0; index < length; index++) {
        uint8_t value = data[index];

        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ value) & 0x01;
            crc >>= 1;

            if (mix != 0) {
                crc ^= 0x8C;
            }

            value >>= 1;
        }
    }

    return crc;
}

esp_err_t ds18b20_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << DS18B20_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t result = gpio_config(&config);

    if (result != ESP_OK) {
        return result;
    }

    return ds18b20_reset() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ds18b20_read_temperature(float *temperature)
{
    if (temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ds18b20_reset()) {
        return ESP_ERR_NOT_FOUND;
    }

    ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
    ds18b20_write_byte(DS18B20_CMD_CONVERT_T);

    vTaskDelay(pdMS_TO_TICKS(DS18B20_CONVERSION_TIME_MS));

    if (!ds18b20_reset()) {
        return ESP_ERR_NOT_FOUND;
    }

    ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
    ds18b20_write_byte(DS18B20_CMD_READ_SCRATCHPAD);

    uint8_t scratchpad[9];

    for (int index = 0; index < 9; index++) {
        scratchpad[index] = ds18b20_read_byte();
    }

    ESP_LOGI(
        TAG,
        "Scratchpad: %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        scratchpad[0], scratchpad[1], scratchpad[2],
        scratchpad[3], scratchpad[4], scratchpad[5],
        scratchpad[6], scratchpad[7], scratchpad[8]
    );

    bool all_zero = true;

    for (int index = 0; index < 9; index++) {
        if (scratchpad[index] != 0) {
            all_zero = false;
            break;
        }
    }

    if (all_zero) {
        ESP_LOGE(TAG, "DS18B20 returned an all-zero scratchpad");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (ds18b20_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGE(TAG, "Invalid DS18B20 scratchpad CRC");
        return ESP_ERR_INVALID_CRC;
    }

    int16_t raw_temperature =
        (int16_t)((scratchpad[1] << 8) | scratchpad[0]);

    *temperature = (float)raw_temperature / 16.0f;
    return ESP_OK;
}
