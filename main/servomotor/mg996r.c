#include "mg996r.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_log.h"

#define MG996R_PWM_FREQUENCY_HZ 50
#define MG996R_PWM_RESOLUTION LEDC_TIMER_13_BIT
#define MG996R_TIMER LEDC_TIMER_0
#define MG996R_CHANNEL LEDC_CHANNEL_0
#define MG996R_SPEED_MODE LEDC_LOW_SPEED_MODE
#define MG996R_MIN_PULSE_US 500
#define MG996R_MAX_PULSE_US 2500
#define MG996R_PWM_PERIOD_US 20000
#define MG996R_MAX_DUTY ((1U << 13) - 1U)

static const char *TAG = "MG996R";
static bool g_initialized;

esp_err_t mg996r_set_angle(int angle)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (angle < 0) {
        angle = 0;
    } else if (angle > 180) {
        angle = 180;
    }

    uint32_t pulse_us =
        MG996R_MIN_PULSE_US +
        ((uint32_t)angle * (MG996R_MAX_PULSE_US - MG996R_MIN_PULSE_US) / 180U);
    uint32_t duty = (pulse_us * MG996R_MAX_DUTY) / MG996R_PWM_PERIOD_US;

    esp_err_t ret = ledc_set_duty(MG996R_SPEED_MODE, MG996R_CHANNEL, duty);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ledc_update_duty(MG996R_SPEED_MODE, MG996R_CHANNEL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Angle %d degrees (%lu us pulse)", angle, (unsigned long)pulse_us);
    }

    return ret;
}

esp_err_t mg996r_init(void)
{
    if (g_initialized) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_config = {
        .speed_mode = MG996R_SPEED_MODE,
        .duty_resolution = MG996R_PWM_RESOLUTION,
        .timer_num = MG996R_TIMER,
        .freq_hz = MG996R_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWM timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = MG996R_SERVO_GPIO,
        .speed_mode = MG996R_SPEED_MODE,
        .channel = MG996R_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = MG996R_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d PWM: %s",
                 MG996R_SERVO_GPIO, esp_err_to_name(ret));
        return ret;
    }

    g_initialized = true;

    ret = mg996r_set_angle(90);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial servo position: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MG996R test servo initialized on GPIO %d", MG996R_SERVO_GPIO);
    return ESP_OK;
}

esp_err_t mg996r_test_start(void)
{
    return mg996r_init();
}