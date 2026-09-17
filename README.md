# Xiaozhi Desk Robot

A single-target ESP32-S3 desk robot firmware derived from
[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32).

This repository is no longer a generic multi-board firmware tree. It is focused on one
physical robot: an ESP32-S3 camera board with voice interaction, animated display,
camera vision, tracked motion, cliff/lift sensing, battery telemetry, MCP device tools,
and a local web control panel.

## What this build does

- Voice assistant using the Xiaozhi protocol stack
- MQTT + UDP and WebSocket protocol implementations
- Opus audio streaming, ASR/LLM/TTS flow, wake-word support
- DVP camera capture and MCP camera vision
- 240×240 ST7789 main display with the custom Mochan robot face
- 128×32 SSD1306 secondary OLED for robot telemetry/status
- Dual N20 motor control through an L298N Mini driver
- MPU6050 motion/gesture sensing and gyro-assisted relative turns
- Downward-facing VL53L0X floor/cliff detection
- INA219 current/power telemetry and persistent battery SoC estimation
- Local Web Control UI for status, motion, camera, display, audio and diagnostics
- Typed Web Chat using the same Xiaozhi session, MCP tools and TTS output
- Device-side MCP tools for robot status, motion, sensors, camera and control
- Persistent robot settings through NVS

## Hardware

Current physical target:

| Part | Current role |
| --- | --- |
| ESP32-S3-WROOM N16R8 camera board | Main controller |
| DVP camera | Vision / MCP camera input |
| INMP441 | I2S microphone |
| MAX98357A-compatible I2S amplifier | Speaker output |
| ST7789 1.5" 240×240 | Main robot-face display |
| SSD1306 0.91" 128×32 | Secondary telemetry OLED |
| L298N Mini + 2× N20 motors | Tracked drive |
| VL53L0X | Downward floor / cliff sensing |
| MPU6050 | Motion, gestures and yaw feedback |
| INA219 | Battery voltage/current/power telemetry |
| TTP223 | Touch / boot control |
| Edison/status LED | Robot status lighting |

### Important buses and GPIOs

The authoritative pin map is
[`main/robot/config/hardware_config.h`](main/robot/config/hardware_config.h).
Behavior thresholds and calibrated runtime values live in
[`main/robot/config/tuning.h`](main/robot/config/tuning.h).

| Function | GPIO |
| --- | --- |
| INMP441 WS / BCLK / DATA | 1 / 2 / 42 |
| Speaker LRCK / BCLK / DATA | 41 / 40 / 39 |
| Touch / boot input | 0 |
| Status LED | 48 |
| Main display SCLK / MOSI / RST / DC / BL | 19 / 20 / 21 / 47 / 45 |
| Left motor IN1 / IN2 | 43 / 44 |
| Right motor IN1 / IN2 | 3 / 46 |
| Primary I2C0 SDA / SCL (camera SCCB + VL53L0X) | 4 / 5 |
| Auxiliary I2C1 SDA / SCL (SSD1306 + INA219 + MPU6050) | 38 / 14 |

Two bus-sharing details are intentional:

- The camera SCCB and VL53L0X reuse one primary I2C0 owner on GPIO4/5.
- SSD1306, INA219 and MPU6050 share the auxiliary I2C1 bus on GPIO38/14.

Do not move these devices casually: GPIO availability on this board is tight and the
camera/PSRAM configuration already consumes most usable pins.

## Architecture

The project keeps the reusable Xiaozhi protocol/audio/application layers, but the board
matrix and unrelated hardware implementations have been removed.

```text
main/
├── application.*                  High-level application/session lifecycle
├── device_state_machine.*         Runtime state transitions
├── protocols/                     MQTT+UDP and WebSocket
├── audio/                         Capture, playback, Opus, wake word and ASR turn control
├── notify/                        Streamed notification playback and lifecycle control
├── display/                       Reusable display infrastructure
├── mcp_server.*                   Device-side MCP framework
├── platform/                      Board, Wi-Fi and hardware adapters
└── robot/                         Desk-robot implementation, adapters, UI, config and tuning
```

The robot-specific subsystem under `main/robot/` owns the motor
controller, Mochan display, secondary OLED, typed MCP/Web adapters, editable Web UI,
battery monitor/SoC, MPU6050 integration and other desk-robot behavior.

`main/application.*` remains the central event/session integrator. The stateful Typed Web Chat
lifecycle and its MCP bridge are isolated in `main/chat/text_chat_controller.*`.
Gemini provider selection, prewarm, VAD, recovery and audio-route switching are isolated in
`main/audio/gemini_asr_turn_controller.*`; `GeminiTranscribeClient` remains the transport client.
Streamed notification playback, subtitle progress and application-state cleanup are isolated in
`main/notify/notification_controller.*`; `NotifyPlayer` remains the HTTP/Ogg playback worker.

The Mochan face keeps one public `MochanDisplay` API while its implementation is split into
core animation/lifecycle, eye-and-mouth raster rendering, and overlay/status presentation.
The secondary OLED likewise keeps one `SecondaryOled` API while low-level glyph and fitted-text
raster primitives live in `secondary_oled_renderer.cc`.

## Typed Web Chat

The local Web Control page can submit text directly into the **existing Xiaozhi
conversation**. It does not create a second chatbot session and does not bypass MCP or
TTS.

Current flow:

```text
short typed message (<= 12 Unicode codepoints)
    -> native detect/text path

long typed message
    -> short trigger: "web_chat"
    -> self.web_chat.consume_pending
    -> original full user text
    -> existing LLM / personality / context
    -> normal MCP tools
    -> normal TTS
    -> robot speaker
```

The Web UI accepts up to **512 Unicode codepoints**.

The controller preserves the existing session, timeout/completion handling and listening
recovery.

When typed chat starts from Idle, the firmware sends one valid Opus silence frame first
to establish the MQTT gateway's UDP return path. This prevents the first TTS response
from being lost while avoiding microphone audio leakage into the typed turn.

## Local Web Control

The desk robot exposes a local control UI on port `8080`.

It provides:

- robot/device status
- motor drive and relative turns
- cliff threshold and safety status
- battery/current/power telemetry
- capacity-test / SoC status
- MPU6050 state
- main-display and secondary-OLED controls
- camera snapshot / lightweight browser preview
- emotion preview
- speaker, microphone and status-light controls
- typed conversation
- live system log

Web Control is split by responsibility:

```text
main/robot/robot_web_control_server.*       HTTP routes, logs, chat/ASR, snapshots
main/robot/web/robot_web_adapter.*          Robot actions and status JSON
main/robot/web/ui/index.html                Editable markup
main/robot/web/ui/style.css                 Editable styling
main/robot/web/ui/app.js                    Editable browser behavior
main/robot/web/robot_web_control_page.h.in  Build-tree generated-page template
```

CMake assembles the three UI source files into a self-contained generated header in the
build tree. Edit the HTML/CSS/JavaScript sources, not generated build output. The firmware
continues to serve the complete page from `/`; no separate asset routes or frontend
toolchain are required.

## MCP

The generic device-side MCP framework is intentionally retained as an extension point.

Robot-specific MCP tools expose hardware state and actions such as camera input, motion,
distance, battery/status and motor-related behavior.

Their robot-facing registration and serialization live in:

```text
main/robot/mcp/robot_mcp_tools.*
```

The generic schema, dispatch and tool framework remains in `main/mcp_server.*`.

Typed Web Chat also uses the AI-visible tool:

```text
self.web_chat.consume_pending
```

Do not remove or hide this tool from the model: it is the long-text bridge for the
official MQTT backend.

## Build

### Requirements

- ESP-IDF v6.1 recommended
- ESP32-S3 target
- Python environment required by ESP-IDF

Source the ESP-IDF environment first:

```bash
source /path/to/esp-idf/export.sh
idf.py --version
```

The project contains one supported physical target:

```text
esp32-s3-camera-robot
```

Canonical configured build:

```bash
python scripts/build.py --language vi-VN
```

Fixed hardware-specific sdkconfig values live in `sdkconfig.robot`; the build helper
combines that fragment with the normal project defaults and requested language/wake word.

Wake word can be selected when needed:

```bash
python scripts/build.py --language vi-VN \
  --wake-word wn9_nihaoxiaozhi_tts
```

Useful discovery commands:

```bash
python scripts/build.py --list-languages
python scripts/build.py --list-wake-words
```

After the project has been configured, normal ESP-IDF commands can be used:

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Development notes

### Single-target repository

Do not reintroduce the upstream multi-board matrix. If another upstream driver or
implementation is needed later, fetch it deliberately from the upstream repository
instead of carrying unused board code here.

The upstream source remains the reference archive:

```text
https://github.com/78/xiaozhi-esp32
```

### Shared I2C initialization

`SharedI2cBus` owns both buses. Primary I2C0 is initialized before the camera, and the
camera SCCB plus downward VL53L0X reuse its existing handle. SSD1306, INA219 and MPU6050
initialization on auxiliary I2C1 remains intentionally deferred and serialized to avoid
startup races. Preserve these ownership and lifecycle rules when adding another device.

### Protocols

MQTT + UDP is the current production transport, but the WebSocket implementation is
intentionally retained. Changes to shared `Protocol` semantics must not assume only one
transport exists.

### OTA / bootstrap

The custom desk-robot target ignores any `firmware` object returned by the bootstrap endpoint.
The existing OTA-named subsystem remains responsible for:

```text
bootstrap / activation / server config / server time
```

No official firmware download, partition-write or auto-upgrade path remains. The only retained
`esp_ota_*` operation marks the currently running image valid after bootstrap succeeds so an
ESP-IDF pending-verify image does not roll back.

## Known limitations / active work

- Battery SoC estimation supports coulomb counting, quasi-rest correction and anchors;
  real-cell calibration remains hardware-dependent.
- The downward VL53L0X is a floor/cliff sensor, not a front obstacle sensor.
- Some large robot implementation files are intentionally left intact for now; future
  modularization should be behavior-preserving rather than a rewrite.

## Upstream

This project is derived from:

- [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)

The reusable protocol, audio, application and MCP foundations originate from that
project. This repository narrows the codebase to the custom ESP32-S3 desk robot and
adds its robot-specific hardware, UI, Web Control, typed chat and behavior.

Keep upstream attribution and the original license when redistributing derived code.

## License

MIT. See [`LICENSE`](LICENSE).
