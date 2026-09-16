# Xiaozhi repository refactor handoff

Updated: 2026-09-16

Branch: `main`

Remote state at handoff: `main` is 12 commits ahead of `origin/main`

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
| `5cd1d84` | Isolate expressive motion planning from the composition root | Build/hardware not run |
| `98e8f44` | Extract Typed Web Chat state and orchestration from `Application` | Build/hardware not run |

Commits `51da4de` through `98e8f44` have not been pushed to `origin`.

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
- `main/robot/motion/expressive_motion_planner.*`: randomized emotion, dance and gyro-look
  movement policy without hardware/task ownership.
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

Typed Web Chat state, the long-message MCP bridge, timeout/completion handling and listening
recovery now live in:

- `main/chat/text_chat_controller.h`
- `main/chat/text_chat_controller.cc`

`Application` remains the session/event-loop integrator and exposes the existing typed-chat
entry points. Its protocol callbacks now forward chat lifecycle events to the controller.

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
- Typed Web Chat still uses native detect for at most 12 Unicode codepoints and
  `self.web_chat.consume_pending` for longer input.
- Idle-origin typed chat still primes the MQTT+UDP return path with deterministic Opus silence,
  never microphone audio, and normal listening resumes after the typed turn.
- No board/release matrix or CI build workflow has been reintroduced.

## Latest unverified batch

Commits `51da4de`, `75ee843`, `c739c47`, `5cd1d84` and `98e8f44` completed Phases 6A
through 8A.
Robot-specific MCP bindings and Web Control action/status adaptation are no longer owned by
`DeskRobotBoard`, and the editable Web UI is no longer maintained in a 3,179-line C++ header.
Phase 7 moved pure randomized emotion/dance/gyro-look policy into
`ExpressiveMotionPlanner`; safety checks, application scheduling and hardware actuation remain
in the composition root. `desk_robot_board.cc` is now 1,448 lines. The remaining auxiliary
sensor loop, cliff response, OLED telemetry, live-camera task and status aggregation were kept
together because they are cross-subsystem lifecycle/integration glue.

Phase 8A moved the Typed Web Chat lifecycle and MCP bridge into `TextChatController` without
changing the public `Application` entry points or protocol contracts. `application.cc` is now
1,885 lines (down from 2,277 immediately before Phase 8A). Gemini ASR turn orchestration remains
in `Application` for the separate Phase 8B batch.

Source review confirmed all 15 MCP tools, all 29 Web actions, all 98 robot status JSON keys, the
nine HTTP routes and port 8080 are retained. The CMake-assembled HTML body matches the previous
embedded body exactly (101,048 characters). Whitespace-insensitive comparison confirmed the
moved emotion, dance and gyro-look policy is unchanged.

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
- `main/robot/motion/expressive_motion_planner.h`
- `main/robot/motion/expressive_motion_planner.cc`
- `main/robot/motion/motion_reactions.h`
- `main/robot/motion/motion_reactions.cc`
- `main/application.h`
- `main/application.cc`
- `main/chat/text_chat_controller.h`
- `main/chat/text_chat_controller.cc`

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
9. Confirm emotion movement, randomized dance, MPU reactions and gyro-assisted emotion turns
   behave as before.
10. Submit typed chat from Idle with both `<= 12` and `> 12` Unicode codepoints; confirm the
    original text is displayed and the normal MCP/LLM/TTS response plays.
11. Submit typed chat while Listening; confirm microphone capture is suppressed for the typed
    turn and listening resumes afterward.
12. Confirm timeout/channel-close failures restore Idle or the prior Listening mode as before.

Build: **NOT RUN**

Hardware: **NOT RUN**

## Next phase

After the user validates Phase 8A, continue with Phase 8B:

```text
main/application.h
main/application.cc
main/audio/gemini_transcribe_client.h
main/audio/gemini_transcribe_client.cc
main/chat/gemini_asr_turn_controller.h
main/chat/gemini_asr_turn_controller.cc
```

Extract only the Gemini ASR turn lifecycle behind a focused controller. Preserve the existing
provider selection, prewarm/cold-start behavior, VAD lifecycle, final-transcript handoff to
Typed Web Chat, retry/timeout recovery and normal Xiaozhi ASR fallback. Do not retune ASR or
reopen Phase 8A.

## Useful review checks

Before committing a future batch:

```bash
git diff --check
git diff --cached --name-status
git diff --cached --stat
```

Do not stage generated/vendor output such as `build/` or `managed_components/`.
