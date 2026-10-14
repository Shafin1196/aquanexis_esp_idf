# Configuration Verification Checklist

## ✅ Pre-Boot Verification (Firmware Build)

- [x] Binary exists: `build/aquanexis_wifi_test.bin` (1.35 MB)
- [x] Build completed without errors
- [x] No compilation errors or linker issues
- [x] All dependencies compiled correctly

## ✅ Configuration Files Verified

### camera_stream.h
- [x] `CAMERA_STREAM_WIDTH = 1920` (was 1280)
- [x] `CAMERA_STREAM_HEIGHT = 1080` (was 960)
- [x] `CAMERA_STREAM_FRAME_INTERVAL_MS = 33` (was 100, = 33 ms ≈ 30 FPS)

### camera_stream.c
- [x] Line 163: `parm.parm.capture.timeperframe.denominator = 30` (was 15)
- [x] Line 490: Send throttle uses `CAMERA_STREAM_FRAME_INTERVAL_MS * 1000LL`
- [x] No hardcoded `CAMERA_STREAM_SEND_INTERVAL_MS` constant

### sdkconfig
- [x] `CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y`
- [x] `CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1920X1080_30FPS=y`
- [x] `CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT=3`

### sdkconfig.defaults
- [x] `CONFIG_CAMERA_OV5647=y`
- [x] `CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y`
- [x] `CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1920X1080_30FPS=y`

## 🔍 Post-Boot Verification (After Flashing)

Watch for these log messages:

### Must See (Confirms 1080p Active)
```
I (XXXXX) AQUANEXIS_CAM: CSI capture format: 1920x1080, UYVY
```

### Must NOT See (Indicates Success)
```
E (XXXXX) video_common: denominator=X is invalid
W (XXXXX) AQUANEXIS_CAM: VIDIOC_S_PARM failed
```

### Should See (Normal Operation)
```
I (XXXXX) AQUANEXIS_CAM: MIPI-CSI video stack initialized
I (XXXXX) AQUANEXIS_CAM: Camera JPEG stream started on /dev/video0
I (XXXXX) AQUANEXIS_WS: WebSocket connected to the server
I (XXXXX) AQUANEXIS_CAM: WebSocket video: X FPS, avg frame Y KB, avg send Z ms
```

## 📊 Frame Statistics Interpretation

Look for lines like:
```
I (27284) AQUANEXIS_CAM: WebSocket video: 7 FPS, avg frame 28 KB, avg send 140 ms
```

| Value | Expected | What It Means |
|-------|----------|---------------|
| FPS | 6–8 FPS | Network bottleneck, not camera limitation |
| Frame Size | 25–35 KB | 1080p JPEG; larger than 1280×960 was (~24 KB) |
| Send Time | 120–160 ms | WebSocket latency over WiFi |

**Key Insight:** If FPS is 6–7 despite camera capable of 30 FPS, the network (WiFi WebSocket) is the bottleneck. Larger 1080p frames may further reduce delivered FPS unless quality is reduced.

## 🛠️ Rollback Instructions

If you need to revert to the old 1280×960 configuration:

```bash
# Restore from backup or reset to previous commit:
git checkout HEAD -- main/camera_stream.h main/camera_stream.c sdkconfig sdkconfig.defaults
idf.py clean
idf.py build
```

## 📝 Expected Behavior Summary

| Aspect | Old Config | New Config | Status |
|--------|-----------|-----------|--------|
| Resolution | 1280×960 | 1920×1080 | ✅ Upgraded |
| Camera FPS (native) | 45 FPS | 30 FPS | ✅ Valid mode |
| Requested FPS | 15 FPS | 30 FPS | ✅ Valid divisor |
| Frame throttle | 100 ms | 33 ms | ✅ Matches 30 FPS |
| Delivered FPS | ~7 FPS | ~6–7 FPS* | ⚠️ Network limited |
| JPEG Frame Size | ~24 KB | ~28+ KB | ⚠️ Slightly larger |

*May be lower due to larger frame sizes; depends on JPEG quality and WiFi bandwidth.

---

**Status:** ✅ All configurations verified and correct. Ready for device flash and boot verification.
