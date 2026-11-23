#include "camera_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"

#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"

#include "websocket_client.h"

#define CAMERA_STREAM_BUFFER_COUNT 2
#define CAMERA_STREAM_JPEG_QUALITY 10
#define CAMERA_STREAM_CAPTURE_FORMAT V4L2_PIX_FMT_RGB565
#define CAMERA_SENDER_TASK_STACK_SIZE 6144

static const char *TAG = "AQUANEXIS_CAM";

static volatile bool g_camera_stream_running = false;
static bool g_stream_started = false;
static bool g_video_stack_initialized = false;

typedef struct {
    bool stop;
    uint8_t slot;
    size_t size;
} camera_send_item_t;

typedef struct {
    QueueHandle_t send_queue;
    QueueHandle_t free_slots;
    uint8_t *buffers[CAMERA_STREAM_BUFFER_COUNT];
    TaskHandle_t owner_task;
} camera_sender_context_t;

#define CAMERA_SCCB_PORT         (1)
#define CAMERA_SCCB_FREQ_HZ      (100000)

/* The board pinout labels SCCB/I2C SCL as GPIO8 and SDA as GPIO7. */
#define CAMERA_SCCB_SCL_PIN      GPIO_NUM_8
#define CAMERA_SCCB_SDA_PIN      GPIO_NUM_7
#define CAMERA_RESET_PIN          GPIO_NUM_NC
#define CAMERA_PWDN_PIN          GPIO_NUM_NC

static esp_err_t camera_init_video_stack(void)
{
    if (g_video_stack_initialized) {
        return ESP_OK;
    }

    esp_video_init_config_t video_cfg = {0};
    esp_video_init_csi_config_t csi_cfg = {0};

    csi_cfg.sccb_config.init_sccb = true;
    csi_cfg.sccb_config.i2c_config.port = CAMERA_SCCB_PORT;
    csi_cfg.sccb_config.i2c_config.scl_pin = CAMERA_SCCB_SCL_PIN;
    csi_cfg.sccb_config.i2c_config.sda_pin = CAMERA_SCCB_SDA_PIN;
    csi_cfg.sccb_config.freq = CAMERA_SCCB_FREQ_HZ;
    csi_cfg.reset_pin = CAMERA_RESET_PIN;
    csi_cfg.pwdn_pin = CAMERA_PWDN_PIN;
    csi_cfg.dont_init_ldo = false;

    video_cfg.csi = &csi_cfg;

    esp_err_t ret = esp_video_init_with_flags(
        &video_cfg,
        ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP | ESP_VIDEO_INIT_FLAGS_JPEG_ENC
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init_with_flags(MIPI_CSI) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    g_video_stack_initialized = true;
    ESP_LOGI(TAG, "MIPI-CSI video stack initialized");
    return ESP_OK;
}

static void camera_print_supported_formats(int fd)
{
    struct v4l2_fmtdesc fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ESP_LOGI(TAG, "Enumerating supported camera formats...");
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        char fourcc[5] = {0};
        memcpy(fourcc, &fmt.pixelformat, 4);

        ESP_LOGI(TAG, "  format: %s (fourcc=%s)",
                 fmt.description,
                 fourcc);

        struct v4l2_frmsizeenum frms = {0};
        frms.index = 0;
        frms.pixel_format = fmt.pixelformat;

        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frms) == 0) {
            if (frms.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                ESP_LOGI(TAG, "    supported size: %ux%u",
                         frms.discrete.width,
                         frms.discrete.height);
            } else if (frms.type == V4L2_FRMSIZE_TYPE_STEPWISE || frms.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                ESP_LOGI(TAG, "    supported range: %ux%u -> %ux%u step %ux%u",
                         frms.stepwise.min_width,
                         frms.stepwise.min_height,
                         frms.stepwise.max_width,
                         frms.stepwise.max_height,
                         frms.stepwise.step_width,
                         frms.stepwise.step_height);
            }
            frms.index++;
        }

        fmt.index++;
    }
}

static esp_err_t camera_setup_capture_device(int fd)
{
    struct v4l2_capability cap = {0};
    struct v4l2_format format = {0};
    struct v4l2_streamparm parm = {0};

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAMERA_STREAM_WIDTH;
    format.fmt.pix.height = CAMERA_STREAM_HEIGHT;
    format.fmt.pix.pixelformat = CAMERA_STREAM_CAPTURE_FORMAT;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CSI capture format: %ux%u, RGB565", format.fmt.pix.width, format.fmt.pix.height);

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = 15;
        if (ioctl(fd, VIDIOC_S_PARM, &parm) != 0) {
            ESP_LOGW(TAG, "Unable to request 15 FPS; continuing with sensor default");
        }
    }

    return ESP_OK;
}

static esp_err_t camera_start_capture(int fd)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t camera_stop_capture(int fd)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed: %s", strerror(errno));
    }

    return ESP_OK;
}

static void camera_sender_task(void *arg)
{
    camera_sender_context_t *context = arg;
    camera_send_item_t item = {0};
    uint32_t frames_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t send_time_us = 0;
    int64_t stats_start_us = esp_timer_get_time();

    while (xQueueReceive(context->send_queue, &item, portMAX_DELAY) == pdPASS) {
        if (item.stop) {
            break;
        }

        int64_t send_start_us = esp_timer_get_time();
        char header[128];
        int header_len = snprintf(
            header,
            sizeof(header),
            "{\"type\":\"camera\",\"codec\":\"jpeg\",\"width\":%d,\"height\":%d,\"size\":%lu}",
            CAMERA_STREAM_WIDTH,
            CAMERA_STREAM_HEIGHT,
            (unsigned long)item.size
        );
        esp_err_t send_result = ESP_FAIL;
        if (header_len > 0 && header_len < (int)sizeof(header)) {
            send_result = websocket_client_send_text_payload(header);
            if (send_result == ESP_OK) {
                send_result = websocket_client_send_binary(context->buffers[item.slot], item.size);
            }
        }

        send_time_us += (uint64_t)(esp_timer_get_time() - send_start_us);
        if (send_result == ESP_OK) {
            frames_sent++;
            bytes_sent += item.size;
        }
        xQueueSend(context->free_slots, &item.slot, portMAX_DELAY);

        int64_t now_us = esp_timer_get_time();
        if (now_us - stats_start_us >= 5000000) {
            uint32_t elapsed_us = (uint32_t)(now_us - stats_start_us);
            uint32_t fps = elapsed_us ? (uint32_t)((uint64_t)frames_sent * 1000000 / elapsed_us) : 0;
            uint32_t average_send_ms = frames_sent ? (uint32_t)(send_time_us / frames_sent / 1000) : 0;
            uint32_t average_frame_kb = frames_sent ? (uint32_t)(bytes_sent / frames_sent / 1024) : 0;
            ESP_LOGI(TAG, "WebSocket video: %u FPS, avg frame %u KB, avg send %u ms",
                     fps, average_frame_kb, average_send_ms);
            frames_sent = 0;
            bytes_sent = 0;
            send_time_us = 0;
            stats_start_us = now_us;
        }
    }

    xTaskNotifyGive(context->owner_task);
    vTaskDelete(NULL);
}

static void camera_stream_task(void *arg)
{
    int camera_fd = -1;
    int encoder_fd = -1;
    bool camera_streaming = false;
    bool encoder_capture_streaming = false;
    bool encoder_output_streaming = false;
    uint8_t *camera_buffers[CAMERA_STREAM_BUFFER_COUNT] = {0};
    size_t camera_buffer_lengths[CAMERA_STREAM_BUFFER_COUNT] = {0};
    uint8_t *jpeg_buffers[CAMERA_STREAM_BUFFER_COUNT] = {0};
    size_t jpeg_buffer_lengths[CAMERA_STREAM_BUFFER_COUNT] = {0};
    camera_sender_context_t sender_context = {0};
    bool sender_task_started = false;
    enum v4l2_buf_type encoder_capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    enum v4l2_buf_type encoder_output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    int64_t last_send_time = 0;

    if (camera_init_video_stack() != ESP_OK) {
        goto cleanup;
    }

    camera_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR | O_NONBLOCK, 0);
    if (camera_fd < 0) {
        ESP_LOGE(TAG, "Open %s failed: %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, strerror(errno));
        goto cleanup;
    }

    camera_print_supported_formats(camera_fd);

    if (camera_setup_capture_device(camera_fd) != ESP_OK) {
        goto cleanup;
    }

    struct v4l2_requestbuffers camera_req = {
        .count = CAMERA_STREAM_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &camera_req) != 0 || camera_req.count < CAMERA_STREAM_BUFFER_COUNT) {
        ESP_LOGE(TAG, "Unable to allocate camera buffers: %s", strerror(errno));
        goto cleanup;
    }

    for (uint32_t index = 0; index < CAMERA_STREAM_BUFFER_COUNT; index++) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = index,
        };
        if (ioctl(camera_fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "Camera VIDIOC_QUERYBUF failed: %s", strerror(errno));
            goto cleanup;
        }
        camera_buffer_lengths[index] = buffer.length;
        camera_buffers[index] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                     camera_fd, buffer.m.offset);
        if (camera_buffers[index] == MAP_FAILED) {
            camera_buffers[index] = NULL;
            ESP_LOGE(TAG, "Camera mmap failed: %s", strerror(errno));
            goto cleanup;
        }
        if (ioctl(camera_fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "Camera VIDIOC_QBUF failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    encoder_fd = open(ESP_VIDEO_JPEG_ENC_DEVICE_NAME, O_RDWR, 0);
    if (encoder_fd < 0) {
        ESP_LOGE(TAG, "Open %s failed: %s", ESP_VIDEO_JPEG_ENC_DEVICE_NAME, strerror(errno));
        goto cleanup;
    }

    struct v4l2_format encoder_format = {0};
    encoder_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    encoder_format.fmt.pix.width = CAMERA_STREAM_WIDTH;
    encoder_format.fmt.pix.height = CAMERA_STREAM_HEIGHT;
    encoder_format.fmt.pix.pixelformat = CAMERA_STREAM_CAPTURE_FORMAT;
    if (ioctl(encoder_fd, VIDIOC_S_FMT, &encoder_format) != 0) {
        ESP_LOGE(TAG, "JPEG encoder input format failed: %s", strerror(errno));
        goto cleanup;
    }

    encoder_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    encoder_format.fmt.pix.width = CAMERA_STREAM_WIDTH;
    encoder_format.fmt.pix.height = CAMERA_STREAM_HEIGHT;
    encoder_format.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    if (ioctl(encoder_fd, VIDIOC_S_FMT, &encoder_format) != 0) {
        ESP_LOGE(TAG, "JPEG encoder output format failed: %s", strerror(errno));
        goto cleanup;
    }

    struct v4l2_ext_control quality_control = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
        .value = CAMERA_STREAM_JPEG_QUALITY,
    };
    struct v4l2_ext_controls quality_controls = {
        .ctrl_class = V4L2_CID_JPEG_CLASS,
        .count = 1,
        .controls = &quality_control,
    };
    if (ioctl(encoder_fd, VIDIOC_S_EXT_CTRLS, &quality_controls) != 0) {
        ESP_LOGW(TAG, "Unable to set JPEG quality: %s", strerror(errno));
    }

    struct v4l2_requestbuffers encoder_output_req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(encoder_fd, VIDIOC_REQBUFS, &encoder_output_req) != 0) {
        ESP_LOGE(TAG, "JPEG encoder input buffer request failed: %s", strerror(errno));
        goto cleanup;
    }

    struct v4l2_requestbuffers encoder_capture_req = {
        .count = CAMERA_STREAM_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(encoder_fd, VIDIOC_REQBUFS, &encoder_capture_req) != 0 ||
            encoder_capture_req.count < CAMERA_STREAM_BUFFER_COUNT) {
        ESP_LOGE(TAG, "JPEG encoder output buffer request failed: %s", strerror(errno));
        goto cleanup;
    }

    for (uint32_t index = 0; index < CAMERA_STREAM_BUFFER_COUNT; index++) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = index,
        };
        if (ioctl(encoder_fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "JPEG encoder VIDIOC_QUERYBUF failed: %s", strerror(errno));
            goto cleanup;
        }
        jpeg_buffer_lengths[index] = buffer.length;
        jpeg_buffers[index] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                   encoder_fd, buffer.m.offset);
        if (jpeg_buffers[index] == MAP_FAILED) {
            jpeg_buffers[index] = NULL;
            ESP_LOGE(TAG, "JPEG encoder mmap failed: %s", strerror(errno));
            goto cleanup;
        }
        if (ioctl(encoder_fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "JPEG encoder VIDIOC_QBUF failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    sender_context.send_queue = xQueueCreate(1, sizeof(camera_send_item_t));
    sender_context.free_slots = xQueueCreate(CAMERA_STREAM_BUFFER_COUNT, sizeof(uint8_t));
    sender_context.owner_task = xTaskGetCurrentTaskHandle();
    if (sender_context.send_queue == NULL || sender_context.free_slots == NULL) {
        ESP_LOGE(TAG, "Unable to allocate video sender queues");
        goto cleanup;
    }
    for (uint8_t index = 0; index < CAMERA_STREAM_BUFFER_COUNT; index++) {
        sender_context.buffers[index] = heap_caps_malloc(
            jpeg_buffer_lengths[index], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (sender_context.buffers[index] == NULL ||
                xQueueSend(sender_context.free_slots, &index, 0) != pdPASS) {
            ESP_LOGE(TAG, "Unable to allocate video sender frame slot %u", index);
            goto cleanup;
        }
    }
    if (xTaskCreate(camera_sender_task, "camera_sender_task", CAMERA_SENDER_TASK_STACK_SIZE,
                    &sender_context, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start video sender task");
        goto cleanup;
    }
    sender_task_started = true;

    if (ioctl(encoder_fd, VIDIOC_STREAMON, &encoder_capture_type) != 0) {
        ESP_LOGE(TAG, "JPEG encoder capture STREAMON failed: %s", strerror(errno));
        goto cleanup;
    }
    encoder_capture_streaming = true;

    if (ioctl(encoder_fd, VIDIOC_STREAMON, &encoder_output_type) != 0) {
        ESP_LOGE(TAG, "JPEG encoder output STREAMON failed: %s", strerror(errno));
        goto cleanup;
    }
    encoder_output_streaming = true;

    if (camera_start_capture(camera_fd) != ESP_OK) {
        goto cleanup;
    }
    camera_streaming = true;

    ESP_LOGI(TAG, "Camera JPEG stream started on %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    g_camera_stream_running = true;

    while (g_camera_stream_running) {
        struct v4l2_buffer camera_buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(camera_fd, VIDIOC_DQBUF, &camera_buf) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (camera_buf.index >= CAMERA_STREAM_BUFFER_COUNT || camera_buf.bytesused == 0 ||
            camera_buf.bytesused > camera_buffer_lengths[camera_buf.index]) {
            ESP_LOGW(TAG, "Ignoring invalid camera buffer index=%u size=%u", camera_buf.index, camera_buf.bytesused);
        } else {
            struct v4l2_buffer encoder_input_buf = {
                .index = 0,
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                .memory = V4L2_MEMORY_USERPTR,
                .m.userptr = (unsigned long)camera_buffers[camera_buf.index],
                .length = camera_buf.bytesused,
                .bytesused = camera_buf.bytesused,
            };
            if (ioctl(encoder_fd, VIDIOC_QBUF, &encoder_input_buf) != 0) {
                ESP_LOGE(TAG, "JPEG encoder input QBUF failed: %s", strerror(errno));
                break;
            }

            struct v4l2_buffer jpeg_buf = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
            };
            struct v4l2_buffer encoder_output_done = {
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                .memory = V4L2_MEMORY_USERPTR,
            };
            if (ioctl(encoder_fd, VIDIOC_DQBUF, &jpeg_buf) != 0 ||
                    ioctl(encoder_fd, VIDIOC_DQBUF, &encoder_output_done) != 0) {
                ESP_LOGE(TAG, "JPEG encoder dequeue failed: %s", strerror(errno));
                break;
            }

            int64_t now = esp_timer_get_time();
            if (jpeg_buf.index < CAMERA_STREAM_BUFFER_COUNT &&
                    jpeg_buf.bytesused > 0 && jpeg_buf.bytesused <= jpeg_buffer_lengths[jpeg_buf.index] &&
                    (now - last_send_time) >= CAMERA_STREAM_FRAME_INTERVAL_MS * 1000LL &&
                    websocket_client_is_connected()) {
                uint8_t slot = 0;
                if (xQueueReceive(sender_context.free_slots, &slot, 0) == pdPASS) {
                    memcpy(sender_context.buffers[slot], jpeg_buffers[jpeg_buf.index], jpeg_buf.bytesused);
                    camera_send_item_t send_item = {
                        .slot = slot,
                        .size = jpeg_buf.bytesused,
                    };
                    if (xQueueSend(sender_context.send_queue, &send_item, 0) == pdPASS) {
                        last_send_time = now;
                    } else {
                        xQueueSend(sender_context.free_slots, &slot, 0);
                    }
                }
            }

            if (jpeg_buf.index < CAMERA_STREAM_BUFFER_COUNT && ioctl(encoder_fd, VIDIOC_QBUF, &jpeg_buf) != 0) {
                ESP_LOGE(TAG, "JPEG encoder output requeue failed: %s", strerror(errno));
                break;
            }

            if (ioctl(camera_fd, VIDIOC_QBUF, &camera_buf) != 0) {
                ESP_LOGE(TAG, "Camera buffer requeue failed: %s", strerror(errno));
                break;
            }
        }
    }

cleanup:
    if (sender_task_started) {
        camera_send_item_t stop_item = {.stop = true};
        if (xQueueSend(sender_context.send_queue, &stop_item, portMAX_DELAY) == pdPASS) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
    if (camera_streaming) {
        camera_stop_capture(camera_fd);
    }
    if (encoder_output_streaming) {
        ioctl(encoder_fd, VIDIOC_STREAMOFF, &encoder_output_type);
    }
    if (encoder_capture_streaming) {
        ioctl(encoder_fd, VIDIOC_STREAMOFF, &encoder_capture_type);
    }
    for (uint32_t index = 0; index < CAMERA_STREAM_BUFFER_COUNT; index++) {
        if (camera_buffers[index] != NULL) {
            munmap(camera_buffers[index], camera_buffer_lengths[index]);
        }
        if (jpeg_buffers[index] != NULL) {
            munmap(jpeg_buffers[index], jpeg_buffer_lengths[index]);
        }
        if (sender_context.buffers[index] != NULL) {
            heap_caps_free(sender_context.buffers[index]);
        }
    }
    if (sender_context.send_queue != NULL) {
        vQueueDelete(sender_context.send_queue);
    }
    if (sender_context.free_slots != NULL) {
        vQueueDelete(sender_context.free_slots);
    }
    if (encoder_fd >= 0) {
        close(encoder_fd);
    }
    if (camera_fd >= 0) {
        close(camera_fd);
    }
    g_stream_started = false;
    g_camera_stream_running = false;
    ESP_LOGI(TAG, "Camera stream task stopped");
    vTaskDelete(NULL);
}

void camera_stream_start(void)
{
    if (g_stream_started) {
        ESP_LOGW(TAG, "Camera stream task already running");
        return;
    }

    if (camera_init_video_stack() != ESP_OK) {
        ESP_LOGE(TAG, "Camera video stack initialization failed; stream not started");
        return;
    }

    g_stream_started = true;
    g_camera_stream_running = true;

    BaseType_t created = xTaskCreate(
        camera_stream_task,
        "camera_stream_task",
        8192,
        NULL,
        5,
        NULL
    );

    if (created != pdPASS) {
        g_stream_started = false;
        g_camera_stream_running = false;
        ESP_LOGE(TAG, "Failed to create camera stream task");
    }
}

void camera_stream_stop(void)
{
    g_camera_stream_running = false;
    g_stream_started = false;
}

bool camera_stream_is_running(void)
{
    return g_stream_started && g_camera_stream_running;
}
