# AGENTS.md

## Project

This repository is the **single-target Xiaozhi Desk Robot firmware**, derived from
`78/xiaozhi-esp32`.

It supports one physical target only:

```text
esp32-s3-camera-robot
ESP32-S3
```

Do not treat this as the old generic multi-board Xiaozhi repository and do not
reintroduce its board/release matrix.

ESP-IDF v6.1 is the preferred SDK.

## Primary ownership

- `main/application.*` — application/session lifecycle, protocol callbacks and subsystem integration.
- `main/chat/text_chat_controller.*` — typed Web Chat state, MCP bridge and session/TTS coordination.
- `main/device_state_machine.*` — legal runtime state transitions.
- `main/protocols/` — shared protocol API, MQTT+UDP and WebSocket.
- `main/audio/` — capture/playback, Opus, wake word and audio tasks.
- `main/audio/gemini_asr_turn_controller.*` — Gemini provider selection, prewarm, VAD and turn lifecycle.
- `main/mcp_server.*` — generic device-side MCP framework.
- `main/platform/` — board, Wi-Fi and reusable hardware adapters.
- `main/robot/config/hardware_config.h` — authoritative wiring, buses and electrical configuration.
- `main/robot/config/tuning.h` — robot behavior thresholds, timing and calibration.
- `main/robot/` — robot-specific hardware and behavior.
- `main/robot/camera/desk_robot_camera.*` — camera capture ownership and concurrency policy.
- `main/robot/control/robot_settings.*` — typed access to persistent robot and audio preferences.
- `main/robot/control/robot_controller.*` and `main/robot/control/robot_status.h` — typed robot control/status boundary.
- `main/robot/mcp/robot_mcp_tools.*` — robot-specific MCP registration and serialization
  through the typed controller boundary.
- `main/robot/display/` — secondary OLED rendering, telemetry orchestration and persisted layout.
- `main/robot/motion/` — gyro-turn lifecycle, expressive movement planning and
  motion-reaction policy.
- `main/robot/sensors/auxiliary_i2c.*` — deferred shared auxiliary-bus ownership and lifecycle.
- `main/robot/sensors/cliff_sensor.*` — downward VL53L0X floor sensing and cliff state.
- `main/robot/power/` — INA219 telemetry, SoC estimation, persistence and capacity tests.
- `main/robot/desk_robot_board.cc` — desk-robot integration.
- `main/robot/motion/motor_controller.*` — drive control.
- `main/robot/display/mochan_display.*` — main robot-face lifecycle and animation orchestration.
- `main/robot/display/mochan_face_renderer.cc` — Mochan eye/mouth geometry and raster rendering.
- `main/robot/display/mochan_display_overlay.cc` — Mochan status, chat, notification, preview and splash UI.
- `main/robot/sensors/mpu6050_motion_sensor.*` — motion sensing.
- `main/robot/robot_web_control_server.*` — local HTTP routes, logs, chat/ASR and
  snapshot transport.
- `main/robot/web/robot_web_adapter.*` — Web action mapping and robot-status JSON
  serialization through `RobotController`.
- `main/robot/web/ui/` — editable Web Control HTML, CSS and JavaScript source.
- `main/robot/web/robot_web_control_page.h.in` — build-tree generated Web Control page
  template; do not edit generated output.
- `main/CMakeLists.txt`, `main/Kconfig.projbuild` and `sdkconfig.robot` — single-target build configuration.
- `scripts/build.py` — configured build helper.

Prefer the narrowest owning subsystem. Do not move desk-robot behavior into generic core
code unless it is genuinely transport/hardware independent.

## Required rules

- Preserve unrelated worktree changes. Keep patches narrow.
- Do not perform opportunistic rewrites during cleanup tasks.
- Do not reintroduce unsupported boards, chip matrices, release matrices or unused drivers.
- Keep reusable abstractions/frameworks when they are useful extension points.
- A build must still export exactly one board factory.
- Change application runtime state through the existing state-machine path.
- Callbacks may run outside the main task; schedule application mutations through the
  established `Application::Schedule()`/event mechanisms.
- Do not block audio tasks or the main application loop.
- Preserve `cJSON` ownership and validate network input.
- Treat NVS keys as persistent state/API; migrate deliberately if renamed.
- Do not edit generated/vendor output such as `build/`, `managed_components/`,
  generated asset headers (including the assembled Web Control header), or generated mmap files.
- Avoid unrelated formatting churn.

## User-owned build / hardware validation

**Do not run builds, flashes, monitors, or physical hardware tests unless the user
explicitly asks you to.**

The user normally performs these steps to save agent time/tokens.

When verification is needed:

1. finish the narrow code/config change;
2. provide only the minimal command/checklist required;
3. stop and wait for the user's PASS/FAIL result or failure log;
4. inspect only the supplied failure output if something breaks.

Never claim physical hardware validation unless the user reports it.

## Hardware constraints that must be preserved

The authoritative GPIO definitions are in:

```text
main/robot/config/hardware_config.h
```

Behavior thresholds and calibrated runtime values are in
`main/robot/config/tuning.h`. Do not retune them during structural changes.

Important shared buses:

```text
GPIO4/5   camera SCCB + downward VL53L0X
GPIO38/14 SSD1306 + INA219 + MPU6050 auxiliary I2C
```

GPIO availability is tight because the camera and octal PSRAM/flash configuration consume
many pins. Do not move or repurpose pins as cleanup.

The auxiliary I2C devices are deliberately initialized later and serially. Do not turn
their initialization back into concurrent boot-time probing.

The VL53L0X points downward and implements floor/cliff/lift safety. Do not treat it as a
front obstacle sensor.

## Protocol rules

Keep both protocol implementations:

```text
MQTT + UDP
WebSocket
```

MQTT+UDP is the current production path, but WebSocket is an intentional extension
reserve.

Shared message semantics belong in `Protocol`. If changing a shared protocol contract,
reason about both transports even if only MQTT+UDP is exercised on the current robot.

## Typed Web Chat

Preserve the current architecture.

The application-owned lifecycle is implemented by `main/chat/text_chat_controller.*` and
integrated through `Application`; do not spread its state back across protocol callbacks.

```text
<= 12 Unicode codepoints
    -> native detect/text

> 12 Unicode codepoints
    -> trigger "web_chat"
    -> self.web_chat.consume_pending
    -> full original text
    -> existing Xiaozhi LLM/MCP/TTS session
```

Web Control accepts up to **512 Unicode codepoints**.

Important invariants:

- Do not create a second chatbot/cloud session.
- Do not replace long-text handling with multi-detect chunking.
- `self.web_chat.consume_pending` must remain AI-visible.
- The original local typed text is the user-visible transcript.
- Idle-origin typed chat primes the MQTT UDP return path with a valid Opus silence frame.
- Do not replace that silence frame with microphone audio.
- After a typed turn, normal listening behavior must recover.

## MCP

Keep the generic MCP server/tool framework as a major extension point.

Remove concrete tools only when their hardware/feature is removed. Do not remove generic
tool registration, argument/schema handling or MCP dispatch during debloat.

Robot hardware status/control should normally extend MCP rather than invent a second
control protocol.

## Display/UI

The main display uses the custom Mochan robot face. The secondary OLED is telemetry/status
oriented.

Avoid duplicating secondary telemetry (battery/current/distance/network status) onto the
main face unless explicitly requested.

Preserve existing face identity and emotion integration; do not create a second emotion
system.

## OTA / bootstrap

The current desk-robot build intentionally ignores official firmware upgrades, but the
existing `Ota` subsystem also carries bootstrap/activation and MQTT/WebSocket/server
configuration.

Do **not** delete the whole subsystem.

The intended cleanup is:

```text
KEEP:
bootstrap
activation
MQTT/WebSocket config
server time
asset behavior unless separately changed

REMOVE/ISOLATE:
official firmware download
firmware partition write
auto-upgrade path
```

Do not change the partition table as part of OTA cleanup unless explicitly requested and
hardware-tested separately.

## Build helper

This is a single-target repository. `scripts/build.py` configures the fixed ESP32-S3
target directly and must not rediscover or validate a nonexistent multi-board tree.

Canonical configured build:

```bash
python scripts/build.py --language vi-VN
```

Useful list commands:

```bash
python scripts/build.py --list-languages
python scripts/build.py --list-wake-words
```

Do not spend agent tokens repeatedly rebuilding or re-auditing completed debloat phases.

## Upstream reference

Upstream is a reference/archive, not code that should be copied wholesale back into this
repository:

```text
https://github.com/78/xiaozhi-esp32
```

When a removed driver/feature is needed later, inspect/fetch the relevant upstream
implementation deliberately and adapt only the required part.

## Validation reporting

For code changes, report:

```text
Changed:
Not changed:
User verification needed:
```

Use `PASS`, `FAIL`, or `NOT RUN` for hardware checks.

A successful compile is not physical hardware validation.
