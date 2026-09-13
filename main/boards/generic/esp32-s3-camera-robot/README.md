# ESP32-S3 Camera Robot

Generic ESP32-S3 WROOM N16R8 camera board wired to an INMP441 microphone, an I2S amplifier,
an ST7789 240x240 primary display, a 0.91-inch SSD1306 128x32 status OLED, an L298N Mini
motor driver, a jumper-A TTP223 touch button, a small Edison status LED, and a VL53L0X
distance sensor. An INA219 power monitor and MPU6050 motion sensor share the secondary OLED's
auxiliary I2C bus.

The board uses the shared desk-robot implementation, including the Mochan-style UI,
camera preview and flip control, queued motor movement and dance action, wake/Wi-Fi
button behavior, audio controls, and the responsive local dashboard on port 8080. The dashboard
also exposes motor/dance telemetry, a microphone level meter, cliff calibration, device health,
browser snapshots, and searchable/downloadable runtime logs.
The camera controls live with the browser preview, and stopping live mode clears its last frame.
The separate Emotions panel can preview every standard Xiaozhi emotion plus the Mochan
`suspicious` and `shake` expressions. The dashboard's **Emotion movement** switch optionally
adds short, safety-guarded body-language sequences to both assistant emotions and previews.
Motor speed, manual-drive duration, and this switch are persisted in the `desk_robot` NVS namespace
(`motor_speed`, `drive_time`, and `emotion_move`); emotion movement defaults off and the
conservative default motor limit is 70%. Four gyro buttons around Forward/Backward provide bounded
left/right 90- and 180-degree turns when the MPU6050 is calibrated and the floor is safe.
Display settings persist in NVS and cover 180-degree rotation of both screens, OLED segment
visibility, custom brand and distance-prefix text, and 1x-3x OLED text scaling. The OLED centers
text that fits its width and only starts the marquee when the composed line overflows.

Build with:

```sh
python3 scripts/build.py generic/esp32-s3-camera-robot --name esp32-s3-camera-robot --language en-US
```

## Display and sensor wiring

- ST7789: SCLK GPIO19, MOSI GPIO20, RST GPIO21, DC GPIO47, CS permanently tied to GND,
  and BL GPIO45. The firmware keeps SPI mode 3 and rotates the panel 270 degrees.
- SSD1306 OLED: SDA GPIO38, SCL GPIO14, powered from 3.3 V. The firmware tries address
  `0x3C` and then `0x3D`. Its brand, assistant state, distance, and battery segments can be
  independently hidden or customized from the dashboard. Text that fits the 128-pixel width is
  centered and static; longer text scrolls continuously.
- INA219: VCC 3.3 V, GND common, SDA GPIO38, SCL GPIO14, address `0x40` with both A0/A1
  jumpers open. The high-side current path is battery positive -> VIN+ -> VIN- -> charger/boost
  battery-positive input. Battery negative remains directly on common GND; VIN- is not GND.
- MPU6050: VCC 3.3 V, GND common, SDA GPIO38, SCL GPIO14. AD0 goes to GND for address
  `0x68`; INT is left disconnected. The driver also probes `0x69` as a fail-soft fallback.
  All three auxiliary devices run at 400 kHz so the animated OLED does not starve sensor reads.
  The current flat mounting uses gyro Z for yaw; `MPU6050_YAW_SIGN` in `config.h` must be flipped
  if a hardware direction test reports positive yaw while the chassis turns left.
- Edison status LED: positive leg GPIO48 and negative leg GND. GPIO48 is driven with PWM;
  the local web dashboard controls the maximum effect brightness. It is off while idle,
  breathes while listening, blinks quickly while speaking, and stays steadily lit for the
  complete duration of a motor command queue. Connecting, Wi-Fi setup, and upgrading use
  distinct blink rates. The optional WS2812 strip that shared this signal in the source wiring
  diagram is not driven by this variant.
- L298N Mini: left motor IN1/IN2 on GPIO43/GPIO44 and right motor IN1/IN2 on
  GPIO3/GPIO46. Native USB Serial/JTAG carries the application console so GPIO43/GPIO44
  remain available to the motors; runtime logs are also available in the local web dashboard.
- Downward-facing VL53L0X: SDA GPIO4 and SCL GPIO5, address `0x29`, powered from 3.3 V.
  It shares the camera SCCB bus; the firmware creates that bus once and passes the same
  handle to the OV3660 camera and distance sensor.

The OLED is a secondary status screen; the ST7789 remains the primary Mochan UI. INA219 data is
smoothed before the estimated one-cell Li-ion percentage, voltage, current, and power are shown on
the displays and dashboard. MPU6050 orientation is calibrated at startup; stable tilts, shaking,
impact/freefall, and extreme orientation can trigger short face reactions only while the robot is
idle and its motors are stopped. Motion-driven expressions can be disabled in the dashboard or
through MCP. The auxiliary sensor task remains the only continuous MPU6050 reader. It publishes
bias-corrected yaw samples for bounded relative turns; unavailable, uncalibrated, or stale gyro
data falls back to timed movement. Missing OLED/INA219/MPU6050 modules are logged but do not stop
boot. The VL53L0X is sampled in a low-priority task, exposed as `self.distance.get`, and included in
the local control status JSON. Its edge threshold defaults to 150 mm, can be calibrated from 50 to
500 mm in the dashboard, and is persisted in NVS. A valid floor reading at or below the threshold
permits forward motion. Two consecutive readings above it, invalid returns, or measurement failures
are treated as a table edge: forward and turning motion stop, the entire queued dance is cancelled,
and those directions remain blocked until the floor is detected again. Reverse remains available so
the robot can escape. A missing VL53L0X is logged but does not stop boot.

The board also exposes expressive MCP tools for conversation-driven behavior:

- `self.face.set_emotion` supports all 21 standard Xiaozhi emotions plus `suspicious` and
  `shake`, with a bounded duration before the face returns to its assistant state.
- `self.face.look` supports the four cardinal and four diagonal gaze directions.
- `self.secondary_display.show_text` temporarily replaces the OLED marquee with a sanitized
  short message, then restores automatic status content.
- `self.status_light.set_effect` temporarily applies `steady`, `breathe`, `blink`, or `off`;
  active motor movement retains priority and the normal status profile resumes afterward.
- `self.battery.get_status` returns INA219 percentage, voltage, current, charge direction,
  and power.
- `self.motion.get_orientation` returns roll, pitch, acceleration magnitude, rotation magnitude,
  the current gesture, and gyro-turn target/progress/result telemetry.
- `self.motion.set_emotion_control` enables or disables automatic MPU6050 face reactions.
- `self.robot.turn_relative` accepts signed degrees from -180 to 180; positive values turn right
  and negative values turn left using the same bounded gyro controller as the dashboard buttons.

Disconnect motor power while flashing or resetting through the onboard USB-UART bridge. Keep the
ESP32, L298N, and motor supply grounds common, and never power the motors from the ESP32 3.3 V rail.
Use a separate motor supply, add a 470-1000 uF bulk capacitor at the driver supply, a 100 nF ceramic
capacitor across each N20 motor, and 10 kOhm pull-down resistors on all four L298N inputs. Firmware
adds an 80 ms break-before-make interval, a 2-second command limit, bounded queued runtime, immediate
timer-task shutdown, and browser focus-loss STOP; these reduce control risk but cannot absorb motor
surges or back-EMF in place of the hardware protection.
