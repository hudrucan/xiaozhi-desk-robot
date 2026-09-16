# Xiaozhi repository refactor handoff

Updated: 2026-09-16

Branch: `main`

Remote state at handoff: `main` is 2 commits ahead of `origin/main`

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

Commit `51da4de` has not been pushed to `origin`.

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

Commit `51da4de` completed Phase 6A by moving all 15 robot-specific MCP tool bindings out of
`DeskRobotBoard`. The new adapter depends on `RobotController`; it does not access board-private
fields or concrete robot subsystem instances. Tool names, schemas, defaults/ranges, feature
guards, response fields and error messages were preserved.

Files changed:

- `main/CMakeLists.txt`
- `main/robot/desk_robot_board.cc`
- `main/robot/mcp/robot_mcp_tools.h`
- `main/robot/mcp/robot_mcp_tools.cc`

Minimum validation:

1. Build with `python scripts/build.py --language vi-VN`.
2. Flash and confirm normal boot.
3. Exercise MCP drive, stop, status, dance and relative turn.
4. Exercise MCP motion orientation/emotion control, distance and battery status.
5. Exercise MCP face emotion/look, secondary-display text, status light, camera flip and
   microphone gain.

Build: **NOT RUN**

Hardware: **NOT RUN**

## Next phase

After the user validates Phase 6A, continue with Phase 6B:

```text
main/robot/robot_web_control_server.h
main/robot/robot_web_control_server.cc
main/robot/desk_robot_board.cc
```

Move Web Control action parsing and status serialization behind `RobotWebControlServer` or a
dedicated Web adapter using `RobotController`. Keep port 8080, every route/action/request field,
all status JSON fields, typed chat, ASR settings, snapshots and logs behavior unchanged.

Do not combine Phase 6C Web UI source extraction with 6A or 6B; it has a separate generated-asset
and browser-regression surface.

## Useful review checks

Before committing a future batch:

```bash
git diff --check
git diff --cached --name-status
git diff --cached --stat
```

Do not stage generated/vendor output such as `build/` or `managed_components/`.
