# Seeed Studio XIAO ESP32-S3 Sense

This variant targets the following wiring:

| Peripheral | XIAO pin / GPIO |
| --- | --- |
| MAX98357A DIN / BCLK / LRC | D4 / GPIO5, D5 / GPIO6, D6 / GPIO43 |
| PDM microphone | GPIO42 clock, GPIO41 data |
| ST7789 SCLK / MOSI / RST / DC | D10 / GPIO9, D9 / GPIO8, D8 / GPIO7, D7 / GPIO44 |
| ST7789 CS | GND (configured as `GPIO_NUM_NC`) |
| Motor driver IN1 / IN2 / IN3 / IN4 | D3 / GPIO4, D2 / GPIO3, D1 / GPIO2, D0 / GPIO1 |

The camera uses the fixed OV3660 pin wiring on the XIAO ESP32-S3 Sense
expansion board. The build selects 8 MB flash and a 240x240 ST7789 display.
The OV3660 uses the DVP `EspVideo` pipeline in YUV422 240x240 mode, matching
the known-good profile encoded in the reference firmware. The UI crops that
frame into the lower response panel while MCP photo uploads use the same frame.

Build after exporting ESP-IDF 6.0.2:

```sh
python3 scripts/build.py seeed/xiao-esp32s3-sense --name xiao-esp32s3-sense --language en-US --wake-word wn9_jarvis_tts
```

This build uses the English UI and the built-in WakeNet phrase `Jarvis`.

The console uses USB Serial/JTAG so UART0 does not claim GPIO43/GPIO44, which
are connected to the amplifier and LCD. Use 115200 baud for the monitor. The
MCP motor tools are `self.robot.drive`, `self.robot.stop`,
`self.robot.get_status`, and `self.robot.dance`. Drive commands are executed in
order (up to 16 queued segments), each has a mandatory automatic stop timeout
capped at 5 seconds, and `self.robot.stop` clears the queue. The camera can be
rotated by 180 degrees with `self.camera.set_camera_flipped`; this preference is
saved across restarts. Speaker volume uses the built-in
`self.audio_speaker.set_volume` tool, while this board adds
`self.audio_microphone.set_gain` for the PDM microphone's integer 1x to 3x
software gain.

After Wi-Fi connects, a local control page is available at
`http://<device-ip>:8080`. It includes press-and-hold drive controls, an
emergency stop, movement duration, Dance, Wake, camera flip, Wi-Fi setup, and
live assistant state. Its Live Camera action shows a low-frame-rate preview in
the ST7789 response panel only while the assistant is idle; MCP photo capture
remains a separate single-frame operation. Starting a conversation switches
Live Camera off, so it stays off after returning to idle until the web button is
toggled again. Speaker volume and microphone gain sliders save their values
when released. The page has no cloud dependency and is intentionally available
only to clients that can reach the device on the local network.

The GPIO0 boot button has two runtime actions: a short press wakes or toggles
chat, while holding it for 3 seconds re-enters Wi-Fi provisioning. Do not hold
GPIO0 while resetting or powering on the board, as that selects the ESP32-S3
bootloader download mode.
