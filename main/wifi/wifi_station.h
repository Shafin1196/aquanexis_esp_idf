#ifndef WIFI_STATION_H
#define WIFI_STATION_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

void wifi_station_start(void);

esp_err_t wifi_station_connect(
    const char *ssid,
    const char *password
);

EventBits_t wifi_station_wait_for_result(
    TickType_t timeout
);

bool wifi_station_is_connected(void);

#endif