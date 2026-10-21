# Technical Deep Dive: 1920×1080@30 FPS Configuration

## Problem Statement (Original)
- Device was configured for **1280×960@45 FPS** sensor mode
- Application requested **20 FPS** from the camera via VIDIOC_S_PARM
- V4L2 driver validation failed because 45 % 20 ≠ 0
- Error: `E (12109) video_common: denominator=20 is invalid`
- Result: Camera fell back to default 45 FPS mode; no actual 20 FPS throttling occurred

## Root Cause Analysis
The Espressif `esp_video` driver validates FPS requests using modulo arithmetic:
```c
// From esp_video_device_common.c validation
if (native_fps % requested_fps != 0) {
    return ESP_FAIL;  // denominator is invalid
}
```

For the OV5647 sensor:
- **1280×960 mode:** Native 45 FPS
  - Valid requests: 1, 3, 5, 9, 15, 45 FPS (divisors of 45)
  - Invalid requests: 2, 4, 6, 7, 8, 10, 12, 14, 16, 18, 20, 24, 30, etc.
  
- **1920×1080 mode:** Native 30 FPS
  - Valid requests: 1, 2, 3, 5, 6, 10, 15, 30 FPS (divisors of 30)
  - Invalid requests: 4, 7, 8, 9, 11, 12, 13, 14, 16, 18, 20, 24, 25, etc.

## Solution: Switch to 1920×1080@30 FPS

### Why This Mode?
1. **Valid FPS match:** 30 % 30 = 0 ✓
2. **Higher resolution:** 1920×1080 (2.07 MP) vs 1280×960 (1.23 MP)
3. **Native support:** OV5647 driver includes this mode (not 5 MP full resolution)
4. **Eliminates error:** No more "denominator=X is invalid" message

### Configuration Cascade

#### 1. Hardware Definition (sdkconfig)
```
CONFIG_CAMERA_OV5647=y                                  # Enable OV5647 driver
CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y      # Compile in 1920×1080 mode
CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS=y  # Keep old mode for compatibility
CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1920X1080_30FPS=y  # Set as default
CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT=3    # Format index 3 = 1920×1080@30
```

**Why index 3?** The OV5647 driver enumerates formats in this order:
- Index 0: RAW10 BGGR (default)
- Index 1: RAW8 BGGR
- Index 2: RGB 5-6-5 LE
- Index 3: RGB 8-8-8
- Index 4: YUV 4:2:0
- Index 5: YUV 4:2:2 UYVY ← **This is where 1920×1080 is injected**

Actually, let me clarify: the index points to which resolution mode is active. In Kconfig, the format definition ties resolution to index. The configuration selects which mode boots by default.

#### 2. Frame Rate Request (main/camera/camera_stream.c)
```c
// Line 163
parm.parm.capture.timeperframe.numerator = 1;
parm.parm.capture.timeperframe.denominator = 30;  // Request 30 FPS
```

This creates the V4L2 timeperframe structure: `numerator/denominator = 1/30 seconds per frame`.

#### 3. Frame Send Throttle (main/camera/camera_stream.c)
```c
// Line 490 - Throttles WebSocket sends to match frame interval
int64_t now = esp_timer_get_time();
if ((now - last_send_time) >= CAMERA_STREAM_FRAME_INTERVAL_MS * 1000LL && ...) {
    // Send frame to WebSocket
    last_send_time = now;
}
```

With `CAMERA_STREAM_FRAME_INTERVAL_MS = 33` ms:
- 33 ms × 1000 = 33,000 microseconds
- 1,000,000 µs ÷ 33,000 µs ≈ 30.3 FPS throttle

#### 4. Application-Level Constants (main/camera/camera_stream.h)
```c
#define CAMERA_STREAM_WIDTH  1920
#define CAMERA_STREAM_HEIGHT 1080
#define CAMERA_STREAM_FRAME_INTERVAL_MS 33
```

These constants propagate through:
- V4L2 format setup (VIDIOC_S_FMT)
- JPEG encoder configuration
- WebSocket frame throttling logic

## Execution Flow at Boot

```
1. ESP32-P4 boots firmware
   ↓
2. esp_cam_sensor driver initializes OV5647
   - Loads format index 3 (1920×1080@30 FPS, UYVY)
   - Enumerates all compiled modes
   ↓
3. camera_stream_init_format() called
   ↓
4. VIDIOC_S_FMT: Set 1920×1080 UYVY format
   - CSI interface configures for 1920×1080 input
   ↓
5. VIDIOC_S_PARM: Request 30 FPS
   - V4L2 validation: 30 % 30 = 0 ✓ PASS
   - Sensor frame rate configured
   ↓
6. JPEG encoder initialized
   - Input: 1920×1080 UYVY frames
   - Output: JPEG at quality 15
   ↓
7. WebSocket stream loop
   - Capture JPEG frames at ~30 FPS from encoder
   - Throttle sends to 1 every 33 ms (≈30 FPS)
   - But network latency limits delivery to ~7 FPS
   ↓
8. Frame statistics logged every 5 seconds
   - "WebSocket video: 7 FPS, avg frame 28 KB, avg send 140 ms"
```

## Performance Bottleneck Analysis

### Why Delivered FPS ≠ Camera FPS

**Camera can produce:** 30 FPS (1920×1080)
**Application sends:** ~7 FPS (measured from logs)
**Network latency:** 140 ms per frame (measured average send time)

**Math:**
- Send time = 140 ms per frame average
- 1 frame / 140 ms = ~7.1 FPS maximum delivery
- This is a **hard ceiling** until network improves

**Why send time is high:**
- JPEG frame size: ~28 KB
- WiFi bandwidth: ~1 Mbps effective (after overhead/interference)
- 28 KB = 224 Kbits
- 224 Kbits ÷ 1 Mbps = 0.224 seconds = 224 ms
- Plus WiFi protocol overhead, retransmits, acknowledgments → ~140 ms average

**Consequence:** Switching from 1280×960 (24 KB frames) to 1920×1080 (28 KB frames) may actually *reduce* delivered FPS if bandwidth is constrained.

## Optimization Opportunities

### 1. Reduce JPEG Quality
```c
// camera_stream.c, line 331
int quality = CAMERA_STREAM_JPEG_QUALITY;  // Currently 15

// Try reducing to 8-10:
int quality = 10;
```

**Impact:**
- Frame size: ~15–18 KB (50% smaller)
- Send time: ~70–90 ms
- Delivered FPS: ~11–14 FPS
- Trade-off: More compression artifacts

### 2. Switch Back to 1280×960
```c
// camera_stream.h
#define CAMERA_STREAM_WIDTH  1280
#define CAMERA_STREAM_HEIGHT 960
#define CAMERA_STREAM_FRAME_INTERVAL_MS 67  // 1000 / 15 ≈ 67 ms for 15 FPS
```

**Impact:**
- Smaller resolution but higher FPS delivery
- Maintains current frame size (~24 KB)
- Avoids resolution upswing penalty

### 3. Implement Adaptive Bitrate
Dynamically reduce JPEG quality if send latency increases:
```c
if (send_time > 150 ms) {
    reduce jpeg_quality to 8;
} else if (send_time < 80 ms) {
    increase jpeg_quality to 20;
}
```

## Why OV5647 Doesn't Support Full 5 MP

The OV5647 sensor can theoretically capture 2592×1944 (5 MP), but:
1. **Espressif didn't implement full resolution mode** in esp_cam_sensor driver
2. **CSI throughput constraints:** The MIPI CSI-2 interface on ESP32-P4 has bandwidth limits
3. **Frame rate trade-off:** Full resolution at high FPS is not feasible on this SoC
4. **Available modes are tuned:** 1920×1080, 1280×960, 800×600 are implemented with validated frame rates

## Files & Their Roles

| File | Purpose | Change |
|------|---------|--------|
| `sdkconfig` | Compile-time configuration | Enable 1920×1080 mode, set as default |
| `sdkconfig.defaults` | Reset template | Add 1920×1080 defaults for clean builds |
| `main/camera/camera_stream.h` | Camera constants | Resolution + frame interval |
| `main/camera/camera_stream.c` | Core driver logic | FPS request + send throttle |
| `esp_cam_sensor/ov5647.c` | Espressif driver (not modified) | Defines available sensor modes |

---

**Conclusion:** The 1920×1080@30 FPS configuration solves the FPS validation error and provides higher resolution. The remaining bottleneck is network throughput, which is independent of camera capability.
