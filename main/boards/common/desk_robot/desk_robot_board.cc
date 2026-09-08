#include "wifi_board.h"

#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_video.h"
#include "led/circular_strip.h"
#include "led/gpio_led.h"
#include "mcp_server.h"
#include "mochan_display.h"
#include "motor_controller.h"
#include "robot_web_control_server.h"
#include "settings.h"

#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#define TAG "DeskRobotBoard"

class DeskRobotEspVideo : public EspVideo {
public:
    explicit DeskRobotEspVideo(const esp_video_init_config_t& config) : EspVideo(config) {}

    bool Capture() override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (mcp_frame_reserved_) {
            return false;
        }
        const bool captured = EspVideo::Capture();
        mcp_frame_reserved_ = captured;
        return captured;
    }

    bool CapturePreview() {
        std::unique_lock<std::mutex> lock(capture_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || mcp_frame_reserved_) {
            return false;
        }
        return EspVideo::Capture();
    }

    std::string Explain(const std::string& question) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        try {
            const std::string result = EspVideo::Explain(question);
            mcp_frame_reserved_ = false;
            return result;
        } catch (...) {
            mcp_frame_reserved_ = false;
            throw;
        }
    }

private:
    std::mutex capture_mutex_;
    bool mcp_frame_reserved_ = false;
};

class DeskRobotBoard : public WifiBoard {
private:
    Button boot_button_;
    MochanDisplay* display_ = nullptr;
    DeskRobotEspVideo* camera_ = nullptr;
    MotorController motors_{MOTOR_LEFT_IN1, MOTOR_LEFT_IN2, MOTOR_RIGHT_IN1, MOTOR_RIGHT_IN2};
    std::unique_ptr<RobotWebControlServer> web_control_server_;
    std::atomic_bool camera_flipped_{false};
    std::atomic_int speaker_volume_{70};
    std::atomic_int microphone_gain_{1};
    std::atomic_bool live_camera_enabled_{false};
    TaskHandle_t live_camera_task_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "") override {
        WifiBoard::OnNetworkEvent(event, data);

        switch (event) {
            case NetworkEvent::Scanning:
            case NetworkEvent::Connecting:
            case NetworkEvent::Disconnected:
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connected:
                display_->SetWifiConnected(true);
                display_->HideBootSplash();
                Application::GetInstance().Schedule([this]() {
                    if (web_control_server_ != nullptr && web_control_server_->Start(8080)) {
                        ESP_LOGI(TAG, "Local control: http://%s:8080",
                                 WifiManager::GetInstance().GetIpAddress().c_str());
                    }
                });
                break;
            case NetworkEvent::WifiConfigModeEnter:
                display_->SetWifiConnected(false);
                display_->HideBootSplash();
                break;
            default:
                break;
        }
    }

    void InitializeSpi() {
        spi_bus_config_t config = {};
        config.mosi_io_num = DISPLAY_MOSI_PIN;
        config.miso_io_num = GPIO_NUM_NC;
        config.sclk_io_num = DISPLAY_SCLK_PIN;
        config.quadwp_io_num = GPIO_NUM_NC;
        config.quadhd_io_num = GPIO_NUM_NC;
        config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        display_ = new MochanDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->ShowBootSplash();
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io =
                {
                    [0] = CAMERA_PIN_D0,
                    [1] = CAMERA_PIN_D1,
                    [2] = CAMERA_PIN_D2,
                    [3] = CAMERA_PIN_D3,
                    [4] = CAMERA_PIN_D4,
                    [5] = CAMERA_PIN_D5,
                    [6] = CAMERA_PIN_D6,
                    [7] = CAMERA_PIN_D7,
                },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = true,
            .i2c_config =
                {
                    .port = 0,
                    .scl_pin = CAMERA_PIN_SIOC,
                    .sda_pin = CAMERA_PIN_SIOD,
                },
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = CAMERA_XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };
        camera_ = new DeskRobotEspVideo(video_config);

        Settings settings("desk_robot", false);
        const bool flipped = settings.GetBool("camera_flip", false);
        camera_flipped_.store(flipped);
        camera_->SetHMirror(flipped);
        camera_->SetVFlip(flipped);
    }

    static bool ParseDirection(const std::string& direction, MotorController::Direction& command) {
        if (direction == "forward") {
            command = MotorController::Direction::kForward;
        } else if (direction == "backward") {
            command = MotorController::Direction::kBackward;
        } else if (direction == "left") {
            command = MotorController::Direction::kLeft;
        } else if (direction == "right") {
            command = MotorController::Direction::kRight;
        } else {
            return false;
        }
        return true;
    }

    void ApplyCameraFlip(bool flipped) {
        camera_->SetHMirror(flipped);
        camera_->SetVFlip(flipped);
        Settings settings("desk_robot", true);
        settings.SetBool("camera_flip", flipped);
    }

    bool QueueCameraFlip() {
        const bool flipped = !camera_flipped_.load();
        camera_flipped_.store(flipped);
        Application::GetInstance().Schedule([this, flipped]() { ApplyCameraFlip(flipped); });
        return flipped;
    }

    void QueueDance() {
        auto& app = Application::GetInstance();
        app.Schedule([this]() {
            motors_.Stop();
            motors_.Drive(MotorController::Direction::kLeft, 260);
            motors_.Drive(MotorController::Direction::kRight, 520);
            motors_.Drive(MotorController::Direction::kLeft, 520);
            motors_.Drive(MotorController::Direction::kRight, 260);
            motors_.Drive(MotorController::Direction::kForward, 220);
            motors_.Drive(MotorController::Direction::kBackward, 220);
        });
    }

    void InitializeAudioSettings() {
        Settings settings("audio", false);
        const int stored_speaker_volume = static_cast<int>(settings.GetInt("output_volume", 70));
        const int stored_microphone_gain = static_cast<int>(settings.GetInt("input_gain", 1));
        const int speaker_volume = std::clamp(stored_speaker_volume, 0, 100);
        const int microphone_gain = std::clamp(stored_microphone_gain, 1, 3);
        speaker_volume_.store(speaker_volume);
        microphone_gain_.store(microphone_gain);
        GetAudioCodec()->SetInputGain(static_cast<float>(microphone_gain));
    }

    void QueueSpeakerVolume(int volume) {
        const int safe_volume = std::clamp(volume, 0, 100);
        speaker_volume_.store(safe_volume);
        Application::GetInstance().Schedule(
            [this, safe_volume]() { GetAudioCodec()->SetOutputVolume(safe_volume); });
    }

    void QueueMicrophoneGain(int gain) {
        const int safe_gain = std::clamp(gain, 1, 3);
        microphone_gain_.store(safe_gain);
        Application::GetInstance().Schedule([this, safe_gain]() {
            GetAudioCodec()->SetInputGain(static_cast<float>(safe_gain));
            Settings settings("audio", true);
            settings.SetInt("input_gain", safe_gain);
        });
    }

    static void LiveCameraTask(void* arg) {
        static_cast<DeskRobotBoard*>(arg)->RunLiveCameraTask();
    }

    void RunLiveCameraTask() {
        bool preview_visible = false;
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            while (live_camera_enabled_.load()) {
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (camera_->CapturePreview()) {
                        preview_visible = true;
                    }
                    vTaskDelay(pdMS_TO_TICKS(250));
                } else {
                    // Live preview is intentionally one-shot per idle session.
                    // Starting a conversation turns the mode off; returning to
                    // idle requires an explicit toggle from the local UI.
                    live_camera_enabled_.store(false);
                    if (preview_visible) {
                        display_->SetPreviewImage(nullptr);
                        preview_visible = false;
                    }
                }
            }
            if (preview_visible) {
                display_->SetPreviewImage(nullptr);
                preview_visible = false;
            }
        }
    }

    void InitializeLiveCamera() {
        BaseType_t result =
            xTaskCreate(LiveCameraTask, "live_camera", 6144, this, 1, &live_camera_task_);
        if (result != pdPASS) {
            live_camera_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create live camera task");
        }
    }

    bool ToggleLiveCamera() {
        if (live_camera_task_ == nullptr) {
            return false;
        }
        const bool enabled = !live_camera_enabled_.load();
        live_camera_enabled_.store(enabled);
        xTaskNotifyGive(live_camera_task_);
        return enabled;
    }

    bool HandleWebAction(const std::string& action, int duration_ms, std::string& message) {
        MotorController::Direction direction;
        if (ParseDirection(action, direction)) {
            const int safe_duration = std::clamp(duration_ms, 50, 2000);
            Application::GetInstance().Schedule([this, direction, safe_duration]() {
                motors_.Stop();
                motors_.Drive(direction, static_cast<uint32_t>(safe_duration));
            });
            message = "Moving " + action;
            return true;
        }
        if (action == "stop") {
            Application::GetInstance().Schedule([this]() { motors_.Stop(); });
            message = "Motors stopped";
            return true;
        }
        if (action == "dance") {
            QueueDance();
            message = "Dance started";
            return true;
        }
        if (action == "wake") {
            Application::GetInstance().ToggleChatState();
            message = "Wake toggled";
            return true;
        }
        if (action == "camera_flip") {
            message = QueueCameraFlip() ? "Camera flipped" : "Camera restored";
            return true;
        }
        if (action == "speaker_volume") {
            const int safe_volume = std::clamp(duration_ms, 0, 100);
            QueueSpeakerVolume(safe_volume);
            message = "Speaker volume " + std::to_string(safe_volume) + "%";
            return true;
        }
        if (action == "microphone_gain") {
            const int safe_gain = std::clamp(duration_ms, 1, 3);
            QueueMicrophoneGain(safe_gain);
            message = "Microphone gain " + std::to_string(safe_gain) + "x";
            return true;
        }
        if (action == "live_camera") {
            const bool enabled = ToggleLiveCamera();
            message = enabled ? "Live preview enabled" : "Live preview disabled";
            return live_camera_task_ != nullptr;
        }
        if (action == "wifi_config") {
            EnterWifiConfigMode();
            message = "Entering Wi-Fi setup";
            return true;
        }
        message = "Unknown action";
        return false;
    }

    void InitializeWebControl() {
        web_control_server_ = std::make_unique<RobotWebControlServer>(
            [this](const std::string& action, int duration_ms, std::string& message) {
                return HandleWebAction(action, duration_ms, message);
            },
            [this]() {
                const char* state =
                    DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
                return std::string("{\"state\":\"") + state +
                       "\",\"camera_flipped\":" + (camera_flipped_.load() ? "true" : "false") +
                       ",\"speaker_volume\":" + std::to_string(speaker_volume_.load()) +
                       ",\"microphone_gain\":" + std::to_string(microphone_gain_.load()) +
                       ",\"live_camera\":" + (live_camera_enabled_.load() ? "true" : "false") + "}";
            });
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress(
            [this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.robot.drive",
            "Drive the two-wheel base. The movement always stops after duration_ms.",
            PropertyList({
                Property("direction", kPropertyTypeString),
                Property("duration_ms", kPropertyTypeInteger, 500, 50, 5000),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const auto direction = properties["direction"].value<std::string>();
                MotorController::Direction command;
                if (!ParseDirection(direction, command)) {
                    throw std::runtime_error("direction must be forward, backward, left, or right");
                }
                const int duration_ms = properties["duration_ms"].value<int>();
                Application::GetInstance().Schedule([this, command, duration_ms]() {
                    motors_.Drive(command, static_cast<uint32_t>(duration_ms));
                });
                return true;
            });
        mcp_server.AddTool("self.robot.stop", "Stop both drive motors immediately.", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               Application::GetInstance().Schedule([this]() { motors_.Stop(); });
                               return true;
                           });
        mcp_server.AddTool(
            "self.robot.get_status", "Get the drive motor state.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return motors_.StatusJson(); });
        mcp_server.AddTool("self.robot.dance", "Run a short dance movement.", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               QueueDance();
                               return true;
                           });
        mcp_server.AddTool("self.camera.set_camera_flipped",
                           "Rotate the camera image by 180 degrees.", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               QueueCameraFlip();
                               return true;
                           });
        mcp_server.AddTool("self.audio_microphone.set_gain",
                           "Set the microphone software gain. Use 1 for normal, 2 for louder, "
                           "or 3 for maximum.",
                           PropertyList({Property("gain", kPropertyTypeInteger, 1, 1, 3)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               QueueMicrophoneGain(properties["gain"].value<int>());
                               return true;
                           });
    }

public:
    DeskRobotBoard() : boot_button_(BOOT_BUTTON_GPIO, BUTTON_ACTIVE_HIGH, 3000) {
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeCamera();
        InitializeAudioSettings();
        InitializeLiveCamera();
        InitializeTools();
        InitializeWebControl();
        if (GetBacklight() != nullptr) {
            GetBacklight()->RestoreBrightness();
        }
        ESP_LOGI(TAG, "Desk robot board initialized");
    }

    AudioCodec* GetAudioCodec() override {
#if AUDIO_MIC_IS_PDM
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                                  AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                                  AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                                  AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#endif
        return &audio_codec;
    }

    Led* GetLed() override {
#if BUILTIN_LED_COUNT > 1
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_COUNT);
#else
        static GpioLed led(BUILTIN_LED_GPIO, true);
#endif
        return &led;
    }

    Backlight* GetBacklight() override {
#if HAS_DISPLAY_BACKLIGHT
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
#else
        return nullptr;
#endif
    }

    Display* GetDisplay() override { return display_; }
    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(DeskRobotBoard);
