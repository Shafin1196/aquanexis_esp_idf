#ifndef TURBIDITY_H
#define TURBIDITY_H

#include "esp_err.h"

typedef struct {
    int raw;
    float sensor_voltage;
} turbidity_sample_t;

esp_err_t turbidity_init(void);
esp_err_t turbidity_read(turbidity_sample_t *sample);

#endif