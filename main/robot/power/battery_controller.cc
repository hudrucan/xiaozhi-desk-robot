#include "battery_controller.h"

#include "config/hardware_config.h"
#include "config/tuning.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cmath>

#define TAG "BatteryController"

BatteryController::BatteryController()
    : soc_estimator_(BatterySocEstimator::Config{
          .usable_capacity_mah = BATTERY_SOC_USABLE_CAPACITY_MAH,
          .maximum_integration_gap_us = BATTERY_SOC_MAX_INTEGRATION_GAP_MS * 1000LL,
          .quasi_rest_max_current_ma = BATTERY_SOC_QUASI_REST_MAX_CURRENT_MA,
          .quasi_rest_current_stddev_ma = BATTERY_SOC_QUASI_REST_CURRENT_STDDEV_MA,
          .quasi_rest_voltage_stddev_v = BATTERY_SOC_QUASI_REST_VOLTAGE_STDDEV_MV / 1000.0f,
          .quasi_rest_current_transition_ma = BATTERY_SOC_QUASI_REST_CURRENT_TRANSITION_MA,
          .quasi_rest_voltage_transition_v =
              BATTERY_SOC_QUASI_REST_VOLTAGE_TRANSITION_MV / 1000.0f,
          .quasi_rest_qualification_us = BATTERY_SOC_QUASI_REST_QUALIFICATION_MS * 1000LL,
          .quasi_rest_correction_interval_us =
              BATTERY_SOC_QUASI_REST_CORRECTION_INTERVAL_MS * 1000LL,
          .quasi_rest_correction_time_constant_us =
              BATTERY_SOC_QUASI_REST_CORRECTION_TIME_CONSTANT_MS * 1000LL,
          .full_anchor_min_voltage_v = BATTERY_SOC_FULL_ANCHOR_MIN_VOLTAGE_V,
          .full_anchor_taper_current_ma = BATTERY_SOC_FULL_ANCHOR_TAPER_CURRENT_MA,
          .full_anchor_qualification_us = BATTERY_SOC_FULL_ANCHOR_QUALIFICATION_MS * 1000LL,
          .bootstrap_full_anchor_min_voltage_soc_percent =
              BATTERY_SOC_BOOTSTRAP_FULL_ANCHOR_MIN_VOLTAGE_SOC_PERCENT,
          .bootstrap_voltage_rebase_min_delta_percent =
              BATTERY_SOC_BOOTSTRAP_VOLTAGE_REBASE_MIN_DELTA_PERCENT,
          .empty_anchor_max_voltage_v = BATTERY_SOC_EMPTY_ANCHOR_MAX_VOLTAGE_V,
          .empty_anchor_qualification_us = BATTERY_SOC_EMPTY_ANCHOR_QUALIFICATION_MS * 1000LL,
      }) {}

void BatteryController::Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex) {
    Settings settings("desk_robot", false);
    capacity_test_active_.store(settings.GetBool("cap_test_on", false));
    capacity_test_uah_.store(
        static_cast<uint32_t>(std::max(settings.GetInt("cap_test_uah", 0), int32_t{0})));
    capacity_test_seconds_.store(
        static_cast<uint32_t>(std::max(settings.GetInt("cap_test_sec", 0), int32_t{0})));

    BatterySocEstimator::PersistedState soc_state;
    soc_state.version = settings.GetInt("soc_ver", 0);
    soc_state.remaining_uah = settings.GetInt("soc_rem_uah", -1);
    soc_state.usable_capacity_uah = settings.GetInt("soc_cap_uah", -1);
    soc_state.soc_basis_points = settings.GetInt("soc_bp", -1);
    soc_state.last_voltage_mv = settings.GetInt("soc_last_mv", -1);
    soc_state.tracking_degraded = settings.GetBool("soc_degraded", false);
    if (soc_estimator_.Restore(soc_state)) {
        remaining_mah_.store(soc_estimator_.GetRemainingMah());
        percent_.store(static_cast<int>(std::lround(soc_estimator_.GetSocPercent())));
        soc_tracking_degraded_.store(soc_estimator_.IsTrackingDegraded());
        ESP_LOGI(TAG, "Battery SoC restored: %.1f mAh (%.2f%%), degraded=%s",
                 soc_estimator_.GetRemainingMah(), soc_estimator_.GetSocPercent(),
                 soc_estimator_.IsTrackingDegraded() ? "yes" : "no");
    } else if (soc_state.version != 0) {
        ESP_LOGW(TAG, "Ignoring incompatible or invalid persisted battery SoC state");
    }

    bus_mutex_ = &bus_mutex;
    std::lock_guard<std::mutex> lock(*bus_mutex_);
    if (bus == nullptr || i2c_master_probe(bus, INA219_I2C_ADDRESS, 100) != ESP_OK) {
        ESP_LOGW(TAG, "INA219 not detected at 0x%02x", INA219_I2C_ADDRESS);
        return;
    }
    if (!power_monitor_.Initialize(bus, INA219_I2C_ADDRESS, INA219_SHUNT_RESISTANCE_OHMS)) {
        ESP_LOGW(TAG, "INA219 initialization failed");
    }
}

void BatteryController::InitializeSamplingState(int64_t now_us) {
    sampling_state_initialized_ = true;
    soc_last_save_us_ = now_us;
    soc_last_saved_remaining_mah_ = soc_estimator_.GetRemainingMah();
    soc_last_saved_degraded_ = soc_estimator_.IsTrackingDegraded();
}

bool BatteryController::Sample(int64_t now_us, bool motors_idle) {
    if (!power_monitor_.IsAvailable() || now_us < next_sample_us_) {
        return false;
    }
    if (!sampling_state_initialized_) {
        InitializeSamplingState(now_us);
    }
    next_sample_us_ = now_us + INA219_SAMPLE_PERIOD_MS * 1000LL;

    Ina219PowerMonitor::Reading reading;
    bool read_ok = false;
    {
        std::lock_guard<std::mutex> lock(*bus_mutex_);
        read_ok = power_monitor_.Read(reading);
    }
    conversion_ready_.store(reading.conversion_ready);
    math_overflow_.store(reading.math_overflow);
    if (!read_ok || !reading.valid) {
        MarkInvalidMeasurement(read_ok, reading);
        return false;
    }

    constexpr float kFilterAlpha = 0.25f;
    if (!filter_initialized_) {
        filtered_voltage_v_ = reading.battery_voltage_v;
        filtered_current_ma_ = reading.current_ma;
        filtered_power_mw_ = reading.power_mw;
        filter_initialized_ = true;
    } else {
        filtered_voltage_v_ += kFilterAlpha * (reading.battery_voltage_v - filtered_voltage_v_);
        filtered_current_ma_ += kFilterAlpha * (reading.current_ma - filtered_current_ma_);
        filtered_power_mw_ += kFilterAlpha * (reading.power_mw - filtered_power_mw_);
    }

    bool seeded_soc = false;
    if (!soc_estimator_.IsInitialized()) {
        soc_estimator_.SeedFromVoltage(reading.battery_voltage_v);
        seeded_soc = true;
        ESP_LOGI(TAG, "Battery SoC seeded from %.3f V: %.2f%%", reading.battery_voltage_v,
                 soc_estimator_.GetSocPercent());
    }
    const float soc_before_update = soc_estimator_.GetSocPercent();
    const bool was_quasi_resting = soc_estimator_.IsQuasiResting();
    const bool was_full_anchored = soc_estimator_.IsFullAnchored();
    const bool was_bootstrap_voltage_rebased = soc_estimator_.WasBootstrapVoltageRebased();
    const bool was_empty_anchored = soc_estimator_.IsEmptyAnchored();
    soc_estimator_.Update(reading.battery_voltage_v, reading.current_ma, motors_idle,
                          reading.charging, now_us);
    if (was_quasi_resting != soc_estimator_.IsQuasiResting()) {
        ESP_LOGI(TAG, "Battery quasi-rest %s: voltage_soc=%.2f%% correction=%+.4f mAh",
                 soc_estimator_.IsQuasiResting() ? "qualified" : "reset",
                 soc_estimator_.GetQuasiRestVoltageSocPercent(),
                 soc_estimator_.GetCumulativeVoltageCorrectionMah());
    }
    const bool full_anchor_applied = !was_full_anchored && soc_estimator_.IsFullAnchored();
    if (full_anchor_applied) {
        ESP_LOGI(TAG, "Battery full anchor qualified at %.3f V, %+.1f mA",
                 reading.battery_voltage_v, reading.current_ma);
    }
    const bool bootstrap_voltage_rebase_applied =
        !was_bootstrap_voltage_rebased && soc_estimator_.WasBootstrapVoltageRebased();
    if (bootstrap_voltage_rebase_applied) {
        ESP_LOGI(TAG, "Battery startup SoC rebased from %.2f%% to %.2f%%", soc_before_update,
                 soc_estimator_.GetSocPercent());
    }
    const bool empty_anchor_applied = !was_empty_anchored && soc_estimator_.IsEmptyAnchored();
    if (empty_anchor_applied) {
        ESP_LOGI(TAG, "Battery empty anchor qualified at %.3f V, %+.1f mA",
                 reading.battery_voltage_v, reading.current_ma);
    }

    voltage_v_.store(filtered_voltage_v_);
    current_ma_.store(std::fabs(filtered_current_ma_));
    power_mw_.store(std::fabs(filtered_power_mw_));
    signed_current_ma_.store(reading.current_ma);
    shunt_voltage_mv_.store(reading.shunt_voltage_mv);
    bus_voltage_v_.store(reading.bus_voltage_v);
    remaining_mah_.store(soc_estimator_.GetRemainingMah());
    percent_.store(std::clamp(static_cast<int>(std::lround(soc_estimator_.GetSocPercent())), 0, 100));
    soc_tracking_degraded_.store(soc_estimator_.IsTrackingDegraded());
    soc_quasi_resting_.store(soc_estimator_.IsQuasiResting());
    soc_voltage_reference_percent_.store(soc_estimator_.GetQuasiRestVoltageSocPercent());
    soc_voltage_correction_mah_.store(soc_estimator_.GetCumulativeVoltageCorrectionMah());
    soc_full_anchored_.store(soc_estimator_.IsFullAnchored());
    soc_bootstrap_voltage_rebased_.store(soc_estimator_.WasBootstrapVoltageRebased());
    soc_empty_anchored_.store(soc_estimator_.IsEmptyAnchored());
    charging_.store(reading.charging);
    discharging_.store(reading.discharging);
    valid_.store(true);

    constexpr int64_t kSocMinimumSaveIntervalUs = 5 * 60 * 1000000LL;
    constexpr float kSocMinimumSaveChangeMah = 10.0f;
    const float remaining_mah = soc_estimator_.GetRemainingMah();
    const bool degraded_changed =
        soc_estimator_.IsTrackingDegraded() != soc_last_saved_degraded_;
    const bool meaningful_change =
        std::fabs(remaining_mah - soc_last_saved_remaining_mah_) >= kSocMinimumSaveChangeMah ||
        degraded_changed;
    if (full_anchor_applied || bootstrap_voltage_rebase_applied || empty_anchor_applied ||
        seeded_soc || (meaningful_change && now_us - soc_last_save_us_ >= kSocMinimumSaveIntervalUs)) {
        const char* save_reason = "periodic";
        if (full_anchor_applied) {
            save_reason = "full anchor";
        } else if (bootstrap_voltage_rebase_applied) {
            save_reason = "startup voltage rebase";
        } else if (empty_anchor_applied) {
            save_reason = "empty anchor";
        } else if (seeded_soc) {
            save_reason = "voltage seed";
        }
        PersistSoc(save_reason);
        soc_last_save_us_ = now_us;
        soc_last_saved_remaining_mah_ = remaining_mah;
        soc_last_saved_degraded_ = soc_estimator_.IsTrackingDegraded();
    }

    UpdateCapacityTest(reading, now_us);
    read_failures_ = 0;
    invalid_samples_ = 0;

    if (now_us < next_display_us_) {
        return false;
    }
    next_display_us_ = now_us + 1000000LL;
    return true;
}

void BatteryController::UpdateCapacityTest(const Ina219PowerMonitor::Reading& reading,
                                           int64_t now_us) {
    const bool capacity_active = capacity_test_active_.load();
    if (capacity_active && !capacity_test_was_active_) {
        capacity_previous_sample_valid_ = false;
        capacity_previous_sample_us_ = 0;
        capacity_previous_current_ma_ = 0.0f;
        capacity_last_save_us_ = now_us;
        capacity_fractional_uah_ = 0.0;
        capacity_fractional_time_us_ = 0;
        capacity_low_voltage_started_us_ = 0;
    }
    const bool capacity_measuring = capacity_active && reading.discharging;
    capacity_test_measuring_.store(capacity_measuring);
    if (capacity_measuring) {
        if (capacity_previous_sample_valid_) {
            const int64_t elapsed_us = now_us - capacity_previous_sample_us_;
            if (elapsed_us > 0 && elapsed_us <= BATTERY_SOC_MAX_INTEGRATION_GAP_MS * 1000LL) {
                const double average_discharge_ma =
                    0.5 * (static_cast<double>(capacity_previous_current_ma_) + reading.current_ma);
                if (average_discharge_ma > 0.0) {
                    capacity_fractional_uah_ += average_discharge_ma * elapsed_us / 3600000.0;
                    const uint32_t whole_uah = static_cast<uint32_t>(capacity_fractional_uah_);
                    if (whole_uah > 0) {
                        capacity_test_uah_.fetch_add(whole_uah);
                        capacity_fractional_uah_ -= whole_uah;
                    }
                    capacity_fractional_time_us_ += elapsed_us;
                    const uint32_t whole_seconds =
                        static_cast<uint32_t>(capacity_fractional_time_us_ / 1000000LL);
                    if (whole_seconds > 0) {
                        capacity_test_seconds_.fetch_add(whole_seconds);
                        capacity_fractional_time_us_ -=
                            static_cast<int64_t>(whole_seconds) * 1000000LL;
                    }
                }
            } else {
                ESP_LOGW(TAG, "Capacity test skipped unknown integration gap: %lld us",
                         static_cast<long long>(elapsed_us));
            }
        }
        capacity_previous_sample_valid_ = true;
        capacity_previous_sample_us_ = now_us;
        capacity_previous_current_ma_ = reading.current_ma;
    } else {
        capacity_previous_sample_valid_ = false;
    }

    if (capacity_measuring && reading.battery_voltage_v <= BATTERY_CAPACITY_LOW_VOLTAGE_V) {
        if (capacity_low_voltage_started_us_ == 0) {
            capacity_low_voltage_started_us_ = now_us;
        }
    } else {
        capacity_low_voltage_started_us_ = 0;
    }
    if (capacity_low_voltage_started_us_ > 0 &&
        now_us - capacity_low_voltage_started_us_ >=
            BATTERY_CAPACITY_LOW_VOLTAGE_DURATION_MS * 1000LL) {
        capacity_test_active_.store(false);
        capacity_test_measuring_.store(false);
        PersistCapacityTest();
        capacity_low_voltage_started_us_ = 0;
        capacity_previous_sample_valid_ = false;
        ESP_LOGI(TAG, "Battery capacity measurement stopped after %d ms at/below %.2f V",
                 BATTERY_CAPACITY_LOW_VOLTAGE_DURATION_MS, BATTERY_CAPACITY_LOW_VOLTAGE_V);
    }
    if (capacity_measuring && now_us - capacity_last_save_us_ >= 60000000LL) {
        PersistCapacityTest();
        capacity_last_save_us_ = now_us;
    }
    capacity_test_was_active_ = capacity_test_active_.load();
}

void BatteryController::MarkInvalidMeasurement(bool read_ok,
                                               const Ina219PowerMonitor::Reading& reading) {
    valid_.store(false);
    capacity_test_measuring_.store(false);
    capacity_previous_sample_valid_ = false;
    capacity_low_voltage_started_us_ = 0;
    soc_estimator_.MarkMeasurementGap();
    soc_tracking_degraded_.store(soc_estimator_.IsTrackingDegraded());
    soc_quasi_resting_.store(false);
    soc_voltage_reference_percent_.store(0.0f);
    if (!read_ok) {
        invalid_samples_ = 0;
        if (++read_failures_ == 1 || read_failures_ % 30 == 0) {
            ESP_LOGW(TAG, "INA219 I2C read failed (%u consecutive)", read_failures_);
        }
    } else {
        read_failures_ = 0;
        if (++invalid_samples_ == 1 || invalid_samples_ % 30 == 0) {
            ESP_LOGW(TAG,
                     "INA219 sample invalid (%u consecutive): conversion_ready=%s "
                     "math_overflow=%s",
                     invalid_samples_, reading.conversion_ready ? "yes" : "no",
                     reading.math_overflow ? "yes" : "no");
        }
    }
}

void BatteryController::PersistCapacityTest() {
    Settings settings("desk_robot", true);
    settings.SetBool("cap_test_on", capacity_test_active_.load());
    settings.SetInt("cap_test_uah", static_cast<int32_t>(capacity_test_uah_.load()));
    settings.SetInt("cap_test_sec", static_cast<int32_t>(capacity_test_seconds_.load()));
}

void BatteryController::PersistSoc(const char* reason) {
    if (!soc_estimator_.IsInitialized()) {
        return;
    }
    const auto state = soc_estimator_.GetPersistedState();
    Settings settings("desk_robot", true);
    settings.SetInt("soc_ver", state.version);
    settings.SetInt("soc_rem_uah", state.remaining_uah);
    settings.SetInt("soc_cap_uah", state.usable_capacity_uah);
    settings.SetInt("soc_bp", state.soc_basis_points);
    settings.SetInt("soc_last_mv", state.last_voltage_mv);
    settings.SetBool("soc_degraded", state.tracking_degraded);
    ESP_LOGI(TAG, "Battery SoC saved (%s): %.1f mAh (%.2f%%), degraded=%s", reason,
             state.remaining_uah / 1000.0f, state.soc_basis_points / 100.0f,
             state.tracking_degraded ? "yes" : "no");
}

bool BatteryController::StartCapacityTest() {
    if (!IsAvailable()) {
        return false;
    }
    capacity_test_active_.store(true);
    PersistCapacityTest();
    return true;
}

void BatteryController::StopCapacityTest() {
    capacity_test_active_.store(false);
    capacity_test_measuring_.store(false);
    PersistCapacityTest();
}

void BatteryController::ResetCapacityTest() {
    capacity_test_active_.store(false);
    capacity_test_measuring_.store(false);
    capacity_test_uah_.store(0);
    capacity_test_seconds_.store(0);
    PersistCapacityTest();
}

BatteryController::Status BatteryController::GetStatus() const {
    return Status{
        .available = IsAvailable(),
        .valid = valid_.load(),
        .percent = percent_.load(),
        .voltage_v = voltage_v_.load(),
        .current_ma = current_ma_.load(),
        .power_mw = power_mw_.load(),
        .signed_current_ma = signed_current_ma_.load(),
        .shunt_voltage_mv = shunt_voltage_mv_.load(),
        .bus_voltage_v = bus_voltage_v_.load(),
        .remaining_mah = remaining_mah_.load(),
        .conversion_ready = conversion_ready_.load(),
        .math_overflow = math_overflow_.load(),
        .soc_tracking_degraded = soc_tracking_degraded_.load(),
        .soc_quasi_resting = soc_quasi_resting_.load(),
        .soc_voltage_reference_percent = soc_voltage_reference_percent_.load(),
        .soc_voltage_correction_mah = soc_voltage_correction_mah_.load(),
        .soc_full_anchored = soc_full_anchored_.load(),
        .soc_bootstrap_voltage_rebased = soc_bootstrap_voltage_rebased_.load(),
        .soc_empty_anchored = soc_empty_anchored_.load(),
        .charging = charging_.load(),
        .discharging = discharging_.load(),
        .capacity_test_active = capacity_test_active_.load(),
        .capacity_test_measuring = capacity_test_measuring_.load(),
        .capacity_test_uah = capacity_test_uah_.load(),
        .capacity_test_seconds = capacity_test_seconds_.load(),
    };
}
