#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_STREAM_WIDTH  1280
#define CAMERA_STREAM_HEIGHT 960
#define CAMERA_STREAM_FORMAT V4L2_PIX_FMT_JPEG
#define CAMERA_STREAM_FRAME_INTERVAL_MS 33

void camera_stream_start(void);
void camera_stream_stop(void);
bool camera_stream_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
