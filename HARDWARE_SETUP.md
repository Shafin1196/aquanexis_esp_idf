# Aquanexis Hardware Setup

This document describes the hardware connections used by the current firmware.
The design is based on an ESP32-P4 host board, an ESP32-C6 module for wireless
connectivity, and an OV5647 MIPI camera.

## Required Hardware

| Device | Firmware use | Supply |
|---|---|---|
| ESP32-P4 host board | Main application and sensors | Board 5 V or USB 5 V |
| ESP32-C6 / ESP-Hosted module | Wi-Fi and BLE | Board/C6 supply |
| OV5647 MIPI camera | MIPI camera stream | Follow the camera carrier-board supply markings |
| MG996R servo | 0-180 degree test servo | 5-6 V servo supply |
| DS18B20 waterproof temperature sensor | Water temperature | 3.3 V |
| Turbidity sensor | Turbidimeter reading | 5 V for the sensor module; 3.3 V maximum at GPIO20 |
| HX711 load-cell amplifier | Weight scale | 3.3-5 V module supply, depending on module rating |
| 1 kg or similarly rated load cell | Weight measurement | Connected to HX711 |
| 10 kOhm resistor | Turbidity divider top resistor | - |
| 20 kOhm resistor | Turbidity divider bottom resistor | - |
| External 5 V servo/BEC supply | Servo power | Minimum 2-3 A capability |

Do not power the MG996R or HX711 current from the ESP32-P4 3.3 V rail.

## ESP32-P4 Pin Map

| Component | Signal | GPIO |
|---|---|---:|
| MG996R servo | PWM/signal | 1 |
| DS18B20 | DATA | 4 |
| HX711 | SCK | 5 |
| HX711 | DOUT | 6 |
| OV5647 | SCCB/SDA | 7 |
| OV5647 | SCCB/SCL | 8 |
| Turbidity sensor | ADC AO | 20 |
| Wi-Fi credential clear button | Signal | 35 |

The GPIO35 button is active-low. Connect it to an external normally-open
button with the other side of the button connected to GND. GPIO35 already has
a firmware pull-up. The code calls this the BOOT button and clears stored
Wi-Fi credentials when it is pressed.

## Common Ground

Connect all grounds together:

- ESP32-P4 GND
- ESP32-C6 board GND
- OV5647/camera board GND
- Servo supply negative
- DS18B20 GND
- Turbidity module GND
- HX711 GND
- Load-cell amplifier ground
- External resistors/divider ground

Never connect the servo positive supply directly to an ESP32 GPIO or to the
P4 3.3 V rail.

## Servo Wiring

| MG996R lead | Connect to |
|---|---|
| Signal/orange | GPIO1 |
| Positive/red | External 5-6 V servo supply positive |
| Ground/brown or black | Servo supply negative and ESP32-P4 GND |

After initialization, the firmware moves the servo to 90 degrees. The PWM
period is 20 ms, and the pulse width ranges from 500 us to 2500 us.

Use a 2-3 A or higher-rated 5-6 V supply for larger servo loads. Add a 470 uF
or larger electrolytic capacitor close to the servo supply positive and
negative terminals if startup or movement causes voltage dips. Observe
capacitor polarity.

## DS18B20 Temperature Sensor

Use a waterproof DS18B20 module if possible.

| DS18B20 lead | Connect to |
|---|---|
| VCC/red | 3.3 V |
| DATA/yellow | GPIO4 |
| GND/black | GND |

Connect a 4.7 kOhm pull-up resistor between GPIO4 and 3.3 V. Keep the signal
wire short and away from the servo power wires.

The firmware waits 750 ms for conversion and validates the scratchpad CRC.

## Turbidity Sensor Wiring

The turbidity module AO signal must pass through a voltage divider before it
is measured. The firmware assumes a maximum sensor AO voltage of 4.5 V and
expects a 10 kOhm top resistor and 20 kOhm bottom resistor.

```text
Sensor AO  (0-4.5 V)
    |
   10 kOhm
    |
    +---------------- GPIO20 / ADC1
    |
   20 kOhm
    |
   GND
```

This arrangement gives approximately 0-3.0 V at GPIO20:

```text
ADC voltage = sensor voltage x 20 / (10 + 20)
```

Other turbidity sensor connections:

| Sensor terminal | Connect to |
|---|---|
| VCC | 5 V module supply |
| GND | GND |
| AO | 10 kOhm top resistor |

Do not apply more than 3.3 V directly to GPIO20. Confirm the divider with a
meter before installing the sensor.

## HX711 and Load Cell

### HX711 Module

| HX711 pin | Connect to |
|---|---|
| VCC | HX711-rated 5 V or 3.3 V supply |
| GND | GND |
| SCK | GPIO5 |
| DOUT | GPIO6 |

Choose the module supply according to the module or breakout-board
datasheet. A 3.3 V supply is safest for ESP32 GPIO compatibility, while
4.5-5.0 V can improve load-cell excitation on some HX711 modules. If SCK or
DOUT reaches 5 V, add appropriate level shifting or use a module whose logic
outputs are 3.3 V-compatible.

The HX711 DOUT line is an input to the ESP32. GPIO5 is used as SCK and GPIO6
is used as DOUT.

### Load Cell

Connect the load cell to the HX711 before applying power:

| Typical load-cell wire | HX711 terminal |
|---|---|
| Red/excitation positive | E+ |
| Black/excitation negative | E- |
| Green/signal positive | A+ |
| White/signal negative | A- |

Some load cells use red/black/green/white or different colors. Identify the
four wires with the load-cell datasheet or a multimeter resistance test.
Do not rely on wire color alone.

The firmware:

- Reads GPIO6 and GPIO5.
- Takes 20 HX711 samples for tare.
- Takes 10 samples for each reported weight.
- Uses a firmware calibration factor of `928.77`.
- Reports weight in grams.

Remove all load from the scale before firmware tare completes. Recalibrate the
constant for the actual load cell and mechanical mounting.

## OV5647 Camera

The OV5647 connects to the ESP32-P4 through MIPI CSI.

- SCCB SDA: GPIO7
- SCCB SCL: GPIO8
- Reset pin: not configured
- Power-down pin: not configured

Use the board's dedicated CSI ribbon connector and a compatible OV5647 carrier.
The camera ribbon must be fully inserted and retain the same pin orientation
shown on the board and camera carrier.

The firmware:

- Enables the OV5647 driver.
- Requests hardware JPEG encoding.
- The active stream constants request 1280x960 capture.
- Requests approximately 15 FPS in the capture device setup.
- Requests JPEG output by the camera stream module.

Camera reset and power-down are managed by board hardware or left disconnected
where the current carrier does not expose them.

## ESP32-C6 and ESP-Hosted

The app is for an ESP32-P4 + ESP32-C6 two-chip setup. Install the ESP32-C6
co-processor firmware and connect it to the ESP32-P4 host according to the
board manufacturer's ESP-Hosted pinout.

Do not guess the ESP-Hosted SDIO/SPI or USB-UART connections. Use the P4/C6
board schematic. The P4 application is configured to use ESP-Hosted for Wi-Fi
and BLE:

- Wi-Fi credentials are provisioned over BLE.
- WebSocket configuration is restored from NVS.
- GPIO35 clears saved setup state.

## Power-On Sequence

1. Disconnect all loads and servo from the assembly.
2. Verify that every ground is common.
3. Verify GPIO20 is limited to 3.3 V or less.
4. Verify camera, load-cell, temperature, and ADC supplies.
5. Apply power to the ESP32-P4/C6 board.
6. Power the servo supply separately.
7. Verify that GPIO1 has not been shorted to 5 V or GND.
8. Apply power to the HX711 and load cell.
9. Check serial logs for initialization messages.
10. Provision Wi-Fi over BLE if required.

## Power Checklist

Before energizing the project, confirm:

- Servo positive is not connected to GPIO1 or the P4 3.3 V pin.
- Servo and logic grounds are connected.
- Turbidity AO is divided before GPIO20.
- DS18B20 data has a 4.7 kOhm pull-up.
- HX711 DOUT/SCK are 3.3 V-compatible.
- Camera uses the MIPI/CSI connector rather than normal GPIO headers.
- The ESP32-C6 is flashed and physically connected for ESP-Hosted.
- No load is on the scale during tare.
