# Quick Reference: 1920×1080@30 FPS Verification

## Expected Boot Sequence

When the device boots, you should see these key log lines:

### ✅ Correct Output (What to Look For)
```
I (12044) AQUANEXIS_CAM: MIPI-CSI video stack initialized
I (12045) AQUANEXIS_CAM: Enumerating supported camera formats...
I (12045) AQUANEXIS_CAM:   format: RAW10 BGGR (fourcc=BG10)
I (12049) AQUANEXIS_CAM:     supported size: 1920x1080
...
I (12129) AQUANEXIS_CAM: CSI capture format: 1920x1080, UYVY
I (12134) AQUANEXIS_WS: WebSocket connected to the server
```

**Key confirmations:**
- Resolution is `1920x1080` ✓
- No "denominator=X is invalid" error ✓
- No "VIDIOC_S_PARM failed" warning ✓
- WebSocket connects successfully ✓

### ❌ Potential Issues & Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `CSI capture format: 1280x960` | Old resolution active | Clear build, rebuild with `idf.py clean` |
| `denominator=20 is invalid` | FPS divisor mismatch | Verify `sdkconfig` has `CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT=3` |
| `VIDIOC_S_PARM failed` | FPS request invalid for mode | Check camera_stream.c line 163 is `denominator = 30` |
| Camera not detected | Missing esp_cam_sensor | Verify `CONFIG_CAMERA_OV5647=y` in sdkconfig |

## Runtime Performance Monitoring

### Frame Statistics (Every 5 Seconds)
```
I (27284) AQUANEXIS_CAM: WebSocket video: X FPS, avg frame Y KB, avg send Z ms
```

**Expected values:**
- **X FPS:** 6–7 FPS (limited by network, not camera)
- **Y KB:** 25–35 KB (1080p JPEG at quality 15)
- **Z ms:** 120–150 ms (WebSocket send latency)

### What Each Metric Means
- **FPS:** Actual frames delivered to server (bottlenecked by network)
- **Avg Frame Size:** JPEG frame size; larger at 1080p than 1280×960
- **Avg Send Time:** Network latency; high values indicate bandwidth saturation

## If FPS Is Still Low (6-7 FPS)

**Root Cause:** Network throughput, not camera
- Previous: 1280×960 frames were ~24 KB; now 1920×1080 frames are ~28+ KB
- WiFi bandwidth: Server must receive ~200+ KB/s for 7 FPS × 30 KB frames
- Latency: 140 ms send time = 7 FPS ceiling regardless of camera capability

**Options to Improve:**
1. **Reduce JPEG quality** (trade: visual quality)
   - Modify `camera_stream.c` line 331: reduce `CAMERA_STREAM_JPEG_QUALITY` (currently 15)
   - Smaller frames = higher FPS delivery

2. **Reduce resolution** (trade: image detail)
   - Switch back to 1280×960, but now with correct 30 FPS request
   - Or implement adaptive resolution switching

3. **Optimize server/network**
   - Ensure server can consume frames at 7+ FPS
   - Check WiFi signal strength (–dBm value in ESP32 logs)
   - Reduce interference, move closer to AP

## Build & Flash Command

If you need to rebuild after changes:
```bash
cd D:\aquanexis_wifi_test
idf.py build
idf.py flash
idf.py monitor
```

## Configuration Location Reference

| Setting | File | Line(s) |
|---------|------|---------|
| Resolution (1920×1080) | `camera_stream.h` | 13–14 |
| Frame interval (33 ms) | `camera_stream.h` | 16 |
| FPS request (30) | `camera_stream.c` | 163 |
| Send throttle logic | `camera_stream.c` | 490 |
| Default format | `sdkconfig` | 4292–4298 |
| JPEG quality | `camera_stream.c` | 331 |

---

**Status:** Firmware built, ready to flash.
**Next:** Monitor boot logs and confirm resolution changes from 1280×960 to 1920×1080.
