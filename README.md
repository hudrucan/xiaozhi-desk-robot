<div align="center">

# 🤖 Xiaozhi Desk Robot

**Purpose-built ESP32-S3 firmware for one expressive, camera-equipped desktop robot.**

[![Firmware](https://img.shields.io/badge/firmware-v2.5.0-7c3aed?style=flat-square)](CMakeLists.txt)
[![Target](https://img.shields.io/badge/target-ESP32--S3-ef4444?style=flat-square&logo=espressif&logoColor=white)](main/CMakeLists.txt)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-6.1-2563eb?style=flat-square&logo=espressif&logoColor=white)](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/)
[![License](https://img.shields.io/badge/license-MIT-16a34a?style=flat-square)](LICENSE)

[Web Control](#web-control) · [Hardware](#hardware) · [Build](#build) · [Architecture](#architecture) · [Companion server](#companion-server)

</div>

---

This is a focused fork of [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32), trimmed to a single physical target:

```text
esp32-s3-n16r8-cam · ESP32-S3 · 16 MB flash · 8 MB octal PSRAM
```

It keeps Xiaozhi's reusable application, audio, protocol and MCP foundations, then adds the hardware control, safety, face, sensors and local dashboard required by the Desk Robot. It is **not** the upstream multi-board firmware tree.

## Highlights

| | Capability | What is included |
| --- | --- | --- |
| 🗣️ | Conversation | Xiaozhi voice sessions, wake word, Opus audio, typed chat and streamed notification playback |
| 🧠 | Speech recognition | Xiaozhi ASR or optional Gemini ASR with prewarm, VAD and turn recovery |
| 👀 | Character | Animated Mochan face, emotions, gaze, Vietnamese glyph fallback and status overlays |
| 🎥 | Vision | DVP camera, safe browser streaming, persistent capture profiles and MCP vision |
| 🛞 | Motion | Differential tracked drive, live joystick, expressive movement and gyro-assisted turns |
| 🛡️ | Safety | Downward cliff sensing, movement interlock and bounded automatic retreat |
| 🌡️ | Telemetry | Battery/SoC, motion, temperature, humidity, pressure and ambient light |
| 🧰 | Control | Responsive local dashboard, server/ASR setup, device health, logs and MCP tools |

## Current scope

| Item | Status |
| --- | --- |
| Hardware target | `esp32-s3-n16r8-cam` only |
| Preferred SDK | ESP-IDF `v6.1` |
| Firmware version | `2.5.0` |
| Languages | `en-US`, `vi-VN` |
| Protocols | MQTT + UDP (production path), WebSocket (retained extension path) |
| Local dashboard | Web Control on port `8080` |
| Device tools | Generic MCP framework plus robot motion, face, camera, display and sensor tools |
| Firmware OTA | Official firmware download/auto-upgrade intentionally absent |
| Bootstrap | Activation, server configuration, server time and running-image validation retained |

## Hardware

| Component | Role |
| --- | --- |
| ESP32-S3-WROOM N16R8 camera board | Main controller |
| DVP camera | Vision and browser preview |
| INMP441 + MAX98357A-compatible amplifier | I2S microphone and speaker |
| 1.5-inch ST7789, 240×240 | Mochan face |
| 0.91-inch SSD1306, 128×32 | Configurable telemetry display |
| L298N Mini + 2× N20 motors | Tracked drive |
| VL53L0X | Downward floor/cliff sensing |
| MPU6050 | Gesture, motion and yaw feedback |
| INA219 | Voltage, current, power and battery SoC |
| AHT20 · BMP280 · BH1750 | Climate, pressure and ambient light |
| TTP223 + status LED | Touch input and robot state lighting |

The complete pin map lives in [`main/robot/config/hardware_config.h`](main/robot/config/hardware_config.h); calibrated behavior belongs in [`main/robot/config/tuning.h`](main/robot/config/tuning.h).

### Shared buses

```text
GPIO 4/5   · I2C0 · camera SCCB + VL53L0X + AHT20 + BMP280 + BH1750
GPIO 38/14 · I2C1 · SSD1306 + INA219 + MPU6050
```

These assignments are intentional. GPIO availability is tight, the camera reuses the primary bus owner, and auxiliary devices are initialized later and serially to avoid boot-time contention. The VL53L0X points **downward** and is a cliff/lift sensor—not a front obstacle sensor.

## Web Control

Open `http://<robot-ip>:8080` after the robot joins Wi-Fi.

The self-contained dashboard is served directly by the firmware and provides:

- live device health, power, environment, motion and safety telemetry;
- button drive, live joystick and gyro-assisted relative turns;
- typed/speech chat, Xiaozhi or Gemini ASR selection and persistent microphone mute;
- camera snapshots, safe live preview and persistent image/capture tuning;
- Mochan emotion preview, display brightness and configurable secondary OLED layouts;
- Xiaozhi bootstrap/server endpoint configuration, audio controls and live logs.

Typed messages use the existing Xiaozhi session, personality, MCP tools and TTS—there is no second chatbot. The browser accepts up to 512 Unicode codepoints; long input is bridged through the AI-visible `self.web_chat.consume_pending` tool.

Web source is editable under [`main/robot/web/ui/`](main/robot/web/ui/). CMake assembles it into a generated build-tree header, so generated output must not be edited.

## Architecture

```text
                         ┌─────────────────────────┐
 Voice / typed text ───▶ │ Application + state     │
                         │ machine                 │
                         └────────────┬────────────┘
                                      │
                 ┌────────────────────┼────────────────────┐
                 ▼                    ▼                    ▼
        Audio / ASR / TTS       Protocol + MCP       Robot controller
        wake word · Gemini      MQTT+UDP · WS        typed status/actions
                 │                    │                    │
                 └────────────────────┼────────────────────┘
                                      ▼
                       face · camera · motors · sensors
                              Web Control · OLED
```

Important ownership boundaries:

- [`main/application.*`](main/application.cc) owns session and application lifecycle.
- [`main/chat/text_chat_controller.*`](main/chat/text_chat_controller.cc) owns typed Web Chat.
- [`main/audio/`](main/audio/) owns capture, playback, wake word and Gemini ASR turns.
- [`main/protocols/`](main/protocols/) contains both MQTT+UDP and WebSocket transports.
- [`main/mcp_server.*`](main/mcp_server.cc) is the reusable device-side MCP framework.
- [`main/robot/`](main/robot/) owns Desk Robot hardware, behavior, adapters and UI.
- [`main/notify/`](main/notify/) owns streamed notification playback and cleanup.

## Build

### Requirements

- ESP-IDF `v6.1` recommended
- Python from the active ESP-IDF environment
- the hardware listed above; there is no board-selection step

```bash
source /path/to/esp-idf/export.sh
idf.py --version

python scripts/build.py --language vi-VN
```

Optional wake-word selection:

```bash
python scripts/build.py --language vi-VN --wake-word wn9_nihaoxiaozhi_tts
```

Discovery helpers:

```bash
python scripts/build.py --list-languages
python scripts/build.py --list-wake-words
```

Once configured, standard ESP-IDF commands remain available:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Fixed target settings are held in [`sdkconfig.robot`](sdkconfig.robot). The helper combines them with the project defaults and the requested language/wake word; it does not rediscover an upstream board matrix.

## Repository map

```text
main/
├── application.*              session and subsystem integration
├── device_state_machine.*     legal runtime transitions
├── audio/                     audio pipeline, wake word and Gemini ASR
├── chat/                      typed Web Chat lifecycle
├── notify/                    streamed notifications
├── protocols/                 MQTT+UDP and WebSocket
├── mcp_server.*               generic MCP schema and dispatch
├── platform/                  reusable board/network/hardware adapters
└── robot/
    ├── camera/                capture ownership and persistent policy
    ├── control/               typed robot status/control boundary
    ├── display/               Mochan face and secondary OLED
    ├── motion/                drive, turns and expressive reactions
    ├── power/                 INA219 and battery SoC
    ├── sensors/               shared I2C, cliff, IMU and environment
    ├── web/                   Web adapter, API serialization and UI source
    └── desk_robot_board.cc    single board integration/factory
```

## Companion server

For a lean local backend tailored to this firmware, see [`hudrucan/xiaozhi-desk-robot-server`](https://github.com/hudrucan/xiaozhi-desk-robot-server).

The firmware can also use a compatible Xiaozhi deployment supplied through its bootstrap endpoint. Server configuration remains separate from device-side robot control: the firmware owns hardware safety and exposes deterministic actions through MCP.

## Notes and limitations

- Battery SoC combines coulomb counting, quasi-rest correction and anchors; real-cell calibration is hardware-specific.
- Environment classifications, pressure trend and adaptive-brightness curves should be validated after final sensor placement.
- Environment sensors are best-effort clients, but a hard SDA/SCL short can still affect every device sharing that bus.
- Do not add upstream board/release matrices or restore removed firmware-update paths to this single-target fork.

## Upstream and license

Derived from [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32). Keep the applicable upstream attribution and license notices when redistributing derived work.

Released under the [MIT License](LICENSE).
