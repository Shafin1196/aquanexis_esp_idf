# ESP32-P4 Aquanexis Camera Upgrade: 1080p@30 FPS Configuration

## Summary
Successfully configured the OV5647 camera to stream at **1920×1080@30 FPS** instead of the previous 1280×960@45 FPS mode.

## Build Status
✅ **Build Successful** (2025-01-31 18:XX:XX UTC)
- Binary size: 1,317,070 bytes (Flash) + 108,778 bytes (DIRAM)
- All dependencies compiled correctly
- No build errors

## Configuration Changes

### 1. Camera Resolution & Frame Interval (`camera_stream.h`)
```c
#define CAMERA_STREAM_WIDTH  1920        // was 1280
#define CAMERA_STREAM_HEIGHT 1080        // was 960
#define CAMERA_STREAM_FRAME_INTERVAL_MS 33  // was 100 (= 33 ms ≈ 30 FPS)
```
**Rationale:** Switches to the OV5647's native 1920×1080@30 FPS mode for higher quality streaming.

### 2. FPS Request & Send Throttle (`camera_stream.c`)
**Line 163:** FPS request changed from 15 to 30
```c
parm.parm.capture.timeperframe.denominator = 30;  // was 15
```

**Line 490:** Send throttle now uses frame interval constant
```c
(now - last_send_time) >= CAMERA_STREAM_FRAME_INTERVAL_MS * 1000LL
```
**Rationale:** 30 FPS is a valid divisor of the 1920×1080 mode's native 30 FPS (30 % 30 = 0). The old 1280×960 mode runs at 45 FPS, so requesting 15 FPS worked (45 % 15 = 0), but the goal was to avoid the "denominator=X is invalid" error.

### 3. Camera Format Selection (`sdkconfig`)
```
CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y
CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1920X1080_30FPS=y
CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT=3
```
**Rationale:** Compiles in the 1920×1080@30 FPS mode and sets it as the default sensor format at boot.

## Expected Boot Log Output

When the device boots with this firmware, you should see:
```
I (12044) AQUANEXIS_CAM: MIPI-CSI video stack initialized
I (12045) AQUANEXIS_CAM: Enumerating supported camera formats...
...
I (12129) AQUANEXIS_CAM: CSI capture format: 1920x1080, UYVY
```

**Key differences from previous logs:**
- Resolution changed from `1280x960` to `1920x1080`
- **No "denominator=20 is invalid" error** (because 30 FPS request matches the mode)
- **No "VIDIOC_S_PARM failed" warning** (because 30 is a valid divisor of 30 FPS)

## Performance Expectations

### Throughput Bottleneck
The primary bottleneck remains **WebSocket send time**, not camera FPS:
- **Previous:** 7 FPS delivered (6–7 FPS, avg frame 24 KB, avg send 140 ms)
- **Expected:** Still limited by network throughput until you optimize:
  - Reduce JPEG quality (currently `CAMERA_STREAM_JPEG_QUALITY = 15`)
  - Reduce per-frame data size (e.g., adaptive scaling)
  - Improve server/network bandwidth

### Sensor Capability
- **Camera:** OV5647 1920×1080@30 FPS (native mode)
- **Encoder:** JPEG at quality 15
- **Network:** WebSocket via WiFi (limited)

The 1920×1080@30 FPS mode provides **higher resolution** than the previous 1280×960@45 FPS, but **delivered FPS to the server will still be ~6–7 FPS** unless network throughput improves.

## Files Modified

| File | Changes |
|------|---------|
| `main/camera_stream.h` | Resolution 1920×1080, frame interval 33 ms |
| `main/camera_stream.c` | FPS request 30, send throttle using frame interval |
| `sdkconfig` | Format index 3 (1920×1080@30), enabled RAW10 mode |
| `sdkconfig.defaults` | Added OV5647 default format configuration |

## Next Steps

1. **Flash the firmware** to the ESP32-P4 board
2. **Monitor the boot output** for `"CSI capture format: 1920x1080, UYVY"` confirmation
3. **Check WebSocket logs** for frame statistics:
   - Monitor FPS delivery vs. frame size
   - Assess if 1080p quality meets your needs
4. **If FPS is still too low**, consider:
   - Reducing JPEG quality further
   - Implementing adaptive resolution scaling
   - Optimizing server-side receive handling

## Validation Checklist

- [x] Build completes without errors
- [x] Binary generated: `build/aquanexis_wifi_test.bin`
- [ ] Device boots and camera initializes to 1920×1080
- [ ] No VIDIOC_S_PARM errors in logs
- [ ] WebSocket stream active with 1080p frames
- [ ] Frame statistics logged (FPS, frame size, send time)
