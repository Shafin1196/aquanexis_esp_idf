#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_err.h"
#include "turbidity.h"

void websocket_client_start(void);
esp_err_t websocket_client_send_temperature(float temperature);
esp_err_t websocket_client_send_turbidity(
	float temperature,
	const turbidity_sample_t *sample
);

#endif
