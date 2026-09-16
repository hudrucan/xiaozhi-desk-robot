#include "auxiliary_i2c.h"

#include <esp_err.h>
#include <esp_log.h>

#define TAG "AuxiliaryI2c"

AuxiliaryI2c::AuxiliaryI2c(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
    : port_(port), sda_(sda), scl_(scl) {}

bool AuxiliaryI2c::InitializeBus() {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = port_,
        .sda_io_num = sda_,
        .scl_io_num = scl_,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true},
    };
    const esp_err_t error = i2c_new_master_bus(&bus_config, &bus_);
    if (error != ESP_OK) {
        bus_ = nullptr;
        ESP_LOGW(TAG, "Cannot create auxiliary I2C bus on SDA GPIO%d/SCL GPIO%d: %s", sda_, scl_,
                 esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(TAG, "Auxiliary I2C bus ready on SDA GPIO%d/SCL GPIO%d", sda_, scl_);
    return true;
}

void AuxiliaryI2c::DeferredInitializationTask(void* arg) {
    static_cast<AuxiliaryI2c*>(arg)->RunDeferredInitialization();
}

void AuxiliaryI2c::RunDeferredInitialization() {
    // This task is queued from Application::Run(), after Application::Initialize() returns.
    // A short delay also lets the main display and audio DMA settle before another driver is added.
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_LOGI(TAG, "Deferred auxiliary I2C initialization starting");

    if (InitializeBus() && initializer_ != nullptr) {
        initializer_(initializer_context_);
    }

    ESP_LOGI(TAG, "Deferred auxiliary I2C initialization complete");
    initializer_ = nullptr;
    initializer_context_ = nullptr;
    initialization_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool AuxiliaryI2c::StartDeferredInitialization(void* context, DeferredInitializer initializer) {
    if (initialization_task_ != nullptr) {
        return true;
    }
    initializer_context_ = context;
    initializer_ = initializer;
    if (xTaskCreate(DeferredInitializationTask, "aux_i2c_init", 10240, this, 1,
                    &initialization_task_) != pdPASS) {
        initialization_task_ = nullptr;
        initializer_ = nullptr;
        initializer_context_ = nullptr;
        ESP_LOGE(TAG, "Failed to create deferred auxiliary I2C initialization task");
        return false;
    }
    return true;
}
