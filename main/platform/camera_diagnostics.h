#pragma once

#include <array>
#include <cstdint>

struct CameraDiagnostics {
    bool available = false;
    bool valid = false;
    int64_t last_read_ms = 0;
    uint32_t register_read_ms = 0;

    bool awb_enabled = false;
    bool awb_gain_enabled = false;
    bool advanced_awb_enabled = false;
    int wb_mode = 0;
    uint8_t wb_control_raw = 0;
    uint16_t awb_r_gain_raw = 0;
    uint16_t awb_g_gain_raw = 0;
    uint16_t awb_b_gain_raw = 0;

    uint8_t isp_control_00_raw = 0;
    uint8_t isp_control_01_raw = 0;
    std::array<uint8_t, 11> ccm_current{};
    std::array<uint8_t, 11> ccm_native{};
    bool ccm_native_valid = false;
    bool ccm_matches_native = false;
    uint32_t awb_table_current_hash = 0;
    uint32_t awb_table_native_hash = 0;
    bool awb_table_native_valid = false;
    bool awb_table_matches_native = false;

    bool aec_enabled = false;
    bool aec2_enabled = false;
    bool agc_enabled = false;
    uint32_t exposure_raw = 0;
    uint8_t gain_raw = 0;
    uint16_t gain_ceiling_raw = 0;
    uint8_t ae_target_high = 0;
    uint8_t ae_target_low = 0;
    uint8_t ae_target_high_2 = 0;
    uint8_t ae_target_low_2 = 0;
    uint8_t ae_fast_high = 0;
    uint8_t ae_fast_low = 0;

    bool bpc_enabled = false;
    bool wpc_enabled = false;
    bool gamma_enabled = false;
    bool lens_correction_enabled = false;
    bool mirror_enabled = false;
    bool flip_enabled = false;
};
