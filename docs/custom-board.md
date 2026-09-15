# Hardware variant guide

This repository is intentionally scoped to one ESP32-S3 camera desk robot. The retained build is:

```sh
python scripts/build.py generic/esp32-s3-camera-robot \
  --name esp32-s3-camera-robot \
  --language en-US \
  --wake-word wn9_hiwalle_tts2
```

## Existing hardware

The compatibility-sensitive identity is `esp32-s3-camera-robot`. Its files are under
`main/boards/generic/esp32-s3-camera-robot/`; shared robot behavior is under
`main/boards/common/desk_robot/`.

Do not change the existing pin map to represent different hardware. That identity is persisted and
reported through OTA. Small changes to the same physical unit belong in its `config.h`; a genuinely
different PCB needs a new uniquely named variant.

## Adding a variant

A new hardware variant requires updating the complete selection chain:

1. Add a board directory with `config.json`, `config.h`, and its board factory source.
2. Give it a unique lowercase `type` and build `name`; choose the real ESP-IDF target.
3. Add the corresponding board symbol in `main/Kconfig.projbuild`.
4. Select its directory and sources in `main/CMakeLists.txt`.
5. Ensure the selected build exports exactly one `DECLARE_BOARD(...)` factory.
6. Add only the drivers and managed-component dependencies that variant actually uses.

Core modules must depend on the `Board` interface rather than a concrete board class or a board
`config.h`. Camera, display, backlight, LED, battery, and sensors remain optional capabilities.

## Runtime rules

- Change application state through `Application::SetDeviceState()`.
- Schedule callbacks that mutate the application with `Application::Schedule()` or event bits.
- Do not block the main event loop or audio tasks.
- Preserve both WebSocket and MQTT/UDP behavior when changing `Protocol` semantics.
- Validate network input, preserve cJSON ownership, and treat NVS keys as persistent API.

## Validation

Use ESP-IDF 6.1 when possible; the minimum supported version is 6.0.1. Build through
`scripts/build.py`, then flash and exercise the affected hardware path. A successful build does not
replace checks of microphone capture, speaker playback, wake word, camera, displays, motors, safety
sensors, Wi-Fi provisioning, reconnect, OTA, and local web control when those paths are changed.
