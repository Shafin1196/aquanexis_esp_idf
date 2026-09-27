#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "turbidity.h"

void websocket_client_start(void);
bool websocket_client_is_connected(void);
esp_err_t websocket_client_send_text_payload(const char *payload);
esp_err_t websocket_client_send_binary(const uint8_t *data, size_t length);
esp_err_t websocket_client_send_temperature(float temperature);
esp_err_t websocket_client_send_turbidity(
	float temperature,
	const turbidity_sample_t *sample
);

#endif
