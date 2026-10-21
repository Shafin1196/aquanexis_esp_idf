#ifndef HX711_SENSOR_H
#define HX711_SENSOR_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t hx711_init(void);
esp_err_t hx711_tare(uint8_t samples);
esp_err_t hx711_read_weight(float *weight_grams, uint8_t samples);

#endif