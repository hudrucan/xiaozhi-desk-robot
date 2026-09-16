# Fixed hardware target

This repository is intentionally scoped to one ESP32-S3 camera desk robot. The retained build is:

```sh
python scripts/build.py --language en-US \
  --wake-word wn9_hiwalle_tts2
```

## Existing hardware

The compatibility-sensitive identity is `esp32-s3-camera-robot`. Its authoritative pin map is
`main/robot/config/hardware_config.h`, its fixed sdkconfig fragment is `sdkconfig.robot`, and robot
behavior is under `main/robot/`.

Runtime thresholds, timing and empirical calibration are isolated in
`main/robot/config/tuning.h`; they are not part of the electrical pin map.

Do not change the existing pin map to represent different hardware. That identity is persisted and
reported through OTA. Small changes to the same physical unit belong in `hardware_config.h`.
Supporting a different PCB is outside this repository's single-target scope.

Core modules must depend on the `Board` interface rather than a concrete board class or the robot
hardware config. Camera, display, backlight, LED, battery, and sensors remain optional capabilities.

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
