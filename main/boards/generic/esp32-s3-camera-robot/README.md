# ESP32-S3 Camera Robot

Generic ESP32-S3 WROOM camera board wired to an INMP441 microphone, an I2S amplifier,
an ST7789 240x240 display, an L298N Mini motor driver, a TTP223 touch button, and an
eight-pixel WS2812 strip.

The board uses the shared desk-robot implementation, including the Mochan-style UI,
camera preview and flip control, queued motor movement and dance action, wake/Wi-Fi
button behavior, audio controls, and the local control page on port 8080.

Build with:

```sh
python3 scripts/build.py generic/esp32-s3-camera-robot --name esp32-s3-camera-robot --language en-US
```

The diagram shows an optional VL53L0X on GPIO4/GPIO5. Those pins are also the camera
SCCB bus, so the sensor is documented but not initialized by this variant yet.
