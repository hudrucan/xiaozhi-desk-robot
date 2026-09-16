# Xiaozhi repository refactor handoff

Updated: 2026-09-16

Branch: `main`

Remote state at handoff: `main` is 8 commits ahead of `origin/main`

Push status: not pushed

## Scope and operating rules

This is a single-target repository for `esp32-s3-camera-robot` on ESP32-S3. Do not
reintroduce the old upstream board or release matrix.

The user owns builds, flashing, monitoring and physical hardware validation. Do not run
those steps unless explicitly requested. The canonical build command is:

```bash
python scripts/build.py --language vi-VN
```

Commit each completed batch locally and do not push `origin`.

## Completed work

| Commit | Refactor batch | Validation |
| --- | --- | --- |
| `3cae96e6` | Flatten single-target source and build layout | User passed |
| `f6f6f082` | Separate hardware configuration from behavior tuning | User passed |
| `e81802e0` | Extract desk-robot camera ownership and concurrency | User passed |
| `88ce08f9` | Centralize deferred auxiliary I2C lifecycle | User passed |
| `65563a74` | Extract battery controller | User passed |
| `dd6acb76` | Extract secondary-display orchestration | User passed |
| `111a7c17` | Remove outdated documentation | Existing repository cleanup commit |
| `0e72f602` | Extract cliff safety, gyro turns, motion reactions and robot settings | User build/hardware check passed |
| `d15a6642` | Add typed robot control and status boundary | User passed |
| `51da4de` | Isolate robot-specific MCP bindings behind `RobotController` | Build/hardware not run |
| `75ee843` | Route Web Control through the typed robot API | Build/hardware not run |
| `c739c47` | Move Web Control UI to split embedded source | Build/browser/hardware not run |
| `b809343` | Document Phase 6 adapter and Web UI ownership | Documentation only |

Commits `51da4de` through `b809343` have not been pushed to `origin`.

## Current architecture

`main/robot/desk_robot_board.cc` remains the composition root. Its hardware and behavior
responsibilities have been split into these owners:

- `main/robot/camera/desk_robot_camera.*`: camera capture ownership and concurrency.
- `main/robot/control/robot_settings.*`: typed access to existing NVS namespaces and keys.
- `main/robot/control/robot_controller.*`: typed robot-control interface implemented by
  `DeskRobotBoard`.
- `main/robot/control/robot_status.h`: coherent immutable status snapshot used by Web and MCP.
- `main/robot/display/mochan_display.*`: main robot-face display.
- `main/robot/display/secondary_display_controller.*`: secondary OLED settings, telemetry and
  update orchestration.
- `main/robot/motion/motor_controller.*`: motor actuation, queue, PWM and safety guard.
- `main/robot/motion/gyro_turn_controller.*`: yaw calibration and relative-turn lifecycle.
- `main/robot/motion/motion_reactions.*`: gesture debounce/cooldown and reaction policy.
- `main/robot/sensors/auxiliary_i2c.*`: deferred shared auxiliary-I2C initialization.
- `main/robot/sensors/cliff_sensor.*`: downward VL53L0X floor/cliff state.
- `main/robot/sensors/mpu6050_motion_sensor.*`: raw/filtered motion acquisition.
- `main/robot/power/battery_controller.*`: battery sampling, SoC, persistence and capacity test.

Web Control actions/status/snapshots and robot MCP callbacks call the typed `RobotController`
boundary. Robot-specific MCP registration and serialization now live in:

- `main/robot/mcp/robot_mcp_tools.h`
- `main/robot/mcp/robot_mcp_tools.cc`

`DeskRobotBoard` now performs one `RobotMcpTools::Register(*this)` composition call and no
longer owns MCP schemas or response serialization.

Web Control HTTP routing, chat/ASR/log handling and status response completion remain in
`RobotWebControlServer`. Robot action mapping and the robot-status JSON payload now live in:

- `main/robot/web/robot_web_adapter.h`
- `main/robot/web/robot_web_adapter.cc`

The Web adapter depends only on `RobotController` plus the config/display types needed to retain
the existing hardware guards and OLED JSON/action contracts. `DeskRobotBoard` only constructs
the server with its typed controller boundary and the application-owned typed-chat callback.

The editable Web UI source is now split by concern:

- `main/robot/web/ui/index.html`
- `main/robot/web/ui/style.css`
- `main/robot/web/ui/app.js`

CMake deterministically assembles these into a generated build-tree header using
`main/robot/web/robot_web_control_page.h.in`. The firmware still serves one self-contained page
from `/`; no asset route or frontend toolchain was added.

`MotorController::Status` is the typed motor snapshot. Its existing JSON contract is retained
through `MotorController::StatusJson`.

## Preserved contracts and invariants

- Existing GPIO assignments and shared-bus wiring are unchanged.
- The camera and downward VL53L0X still share GPIO4/GPIO5.
- SSD1306, INA219 and MPU6050 still initialize serially and later on GPIO38/GPIO14.
- Cliff confirmation, emergency stop and forward auto-retreat behavior are unchanged.
- Gyro yaw axis/sign, calibration constants, timeout and PWM taper are unchanged.
- Gesture, press reaction, dance and emotion-movement behavior are unchanged.
- Existing NVS namespaces and keys are unchanged.
- Web routes, request parameters and JSON keys are unchanged.
- Robot MCP tool names, schemas and response formats are unchanged.
- Both MQTT+UDP and WebSocket protocol implementations remain present.
- No board/release matrix or CI build workflow has been reintroduced.

## Latest unverified batch

Commits `51da4de`, `75ee843` and `c739c47` completed Phases 6A through 6C. Robot-specific MCP
bindings and Web Control action/status adaptation are no longer owned by `DeskRobotBoard`, and
the editable Web UI is no longer maintained in a 3,179-line C++ header. Source review confirmed
all 15 MCP tools, all 29 Web actions, all 98 robot status JSON keys, the nine HTTP routes and port
8080 are retained. The CMake-assembled HTML body matches the previous embedded body exactly
(101,048 characters).

Files changed:

- `main/CMakeLists.txt`
- `main/robot/desk_robot_board.cc`
- `main/robot/mcp/robot_mcp_tools.h`
- `main/robot/mcp/robot_mcp_tools.cc`
- `main/robot/robot_web_control_server.h`
- `main/robot/robot_web_control_server.cc`
- `main/robot/web/robot_web_adapter.h`
- `main/robot/web/robot_web_adapter.cc`
- `main/robot/web/robot_web_control_page.h.in`
- `main/robot/web/ui/index.html`
- `main/robot/web/ui/style.css`
- `main/robot/web/ui/app.js`
- removed `main/robot/robot_web_control_page.h`

Minimum validation:

1. Build with `python scripts/build.py --language vi-VN`.
2. Flash and confirm normal boot.
3. Exercise MCP drive, stop, status, dance and relative turn.
4. Exercise MCP motion orientation/emotion control, distance and battery status.
5. Exercise MCP face emotion/look, secondary-display text, status light, camera flip and
   microphone gain.
6. Open Web Control and verify status, logs, camera snapshot, typed chat and ASR settings.
7. Exercise movement, turn, dance, emotion, audio test, display/OLED, lighting, battery-test,
   cliff, motion, live-camera and Wi-Fi actions used by the Web UI.
8. Confirm the Web page loads with styling and JavaScript behavior intact, including status
   polling, logs, camera preview/snapshot and typed chat.

Build: **NOT RUN**

Hardware: **NOT RUN**

## Next phase

After the user validates Phases 6A through 6C, continue with Phase 7:

```text
main/robot/desk_robot_board.cc
```

Re-audit the remaining composition root by responsibility. Extract only ownership boundaries
that are clearly cohesive and preserve shared-bus initialization, task scheduling, safety and
telemetry timing. Stop if the remaining length is justified integration/lifecycle code; do not
split solely to meet a line-count target.

## Useful review checks

Before committing a future batch:

```bash
git diff --check
git diff --cached --name-status
git diff --cached --stat
```

Do not stage generated/vendor output such as `build/` or `managed_components/`.
