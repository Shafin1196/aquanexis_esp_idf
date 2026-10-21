#include <stdbool.h>

#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "soc/adc_channel.h"

#include "turbidity.h"

#define TURBIDITY_ADC_UNIT ADC_UNIT_1
#define TURBIDITY_ADC_CHANNEL ADC1_GPIO20_CHANNEL
#define TURBIDITY_ADC_ATTENUATION ADC_ATTEN_DB_12

/* AO is divided from 0-4.5 V to approximately 0-3.0 V. */
#define TURBIDITY_DIVIDER_TOP_OHMS 10000.0f
#define TURBIDITY_DIVIDER_BOTTOM_OHMS 20000.0f
#define TURBIDITY_SAMPLE_COUNT 8

static const char *TAG = "TURBIDITY";
static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t calibration_handle;
static bool calibration_enabled;

esp_err_t turbidity_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = TURBIDITY_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t result = adc_oneshot_new_unit(&unit_config, &adc_handle);
    if (result != ESP_OK) {
        return result;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = TURBIDITY_ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    result = adc_oneshot_config_channel(
        adc_handle,
        TURBIDITY_ADC_CHANNEL,
        &channel_config
    );
    if (result != ESP_OK) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return result;
    }

    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = TURBIDITY_ADC_UNIT,
        .chan = TURBIDITY_ADC_CHANNEL,
        .atten = TURBIDITY_ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    result = adc_cali_create_scheme_curve_fitting(
        &calibration_config,
        &calibration_handle
    );
    if (result == ESP_OK) {
        calibration_enabled = true;
        ESP_LOGI(TAG, "ADC initialized on GPIO20 with calibration");
    } else {
        calibration_enabled = false;
        ESP_LOGW(
            TAG,
            "ADC calibration unavailable (%s); using nominal ADC scale",
            esp_err_to_name(result)
        );
    }

    return ESP_OK;
}

esp_err_t turbidity_read(turbidity_sample_t *sample)
{
    if (sample == NULL || adc_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_total = 0;
    for (int index = 0; index < TURBIDITY_SAMPLE_COUNT; index++) {
        int raw;
        esp_err_t result = adc_oneshot_read(
            adc_handle,
            TURBIDITY_ADC_CHANNEL,
            &raw
        );
        if (result != ESP_OK) {
            return result;
        }
        raw_total += raw;
    }

    sample->raw = raw_total / TURBIDITY_SAMPLE_COUNT;

    int adc_voltage_mv;
    esp_err_t result;
    if (calibration_enabled) {
        result = adc_cali_raw_to_voltage(
            calibration_handle,
            sample->raw,
            &adc_voltage_mv
        );
    } else {
        adc_voltage_mv = (sample->raw * 3300) / 4095;
        result = ESP_OK;
    }

    if (result != ESP_OK) {
        return result;
    }

    sample->sensor_voltage =
        ((float)adc_voltage_mv / 1000.0f) *
        ((TURBIDITY_DIVIDER_TOP_OHMS + TURBIDITY_DIVIDER_BOTTOM_OHMS) /
         TURBIDITY_DIVIDER_BOTTOM_OHMS);

    return ESP_OK;
}