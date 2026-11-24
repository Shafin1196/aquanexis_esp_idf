#ifndef MG996R_H
#define MG996R_H

#include "esp_err.h"

#define MG996R_SERVO_GPIO 1

esp_err_t mg996r_init(void);
esp_err_t mg996r_set_angle(int angle);
esp_err_t mg996r_test_start(void);

#endif