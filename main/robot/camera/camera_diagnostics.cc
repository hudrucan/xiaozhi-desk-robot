#include "camera_diagnostics.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>

namespace {

constexpr char kTag[] = "CameraDiagnostics";
constexpr uint32_t kMarkerMagic = 0x43414D44;  // "CAMD"
constexpr uint32_t kChecksumSalt = 0xA59F37C1;

struct RetainedCameraMarker {
    uint32_t magic;
    uint32_t operation_id;
    uint32_t stage;
    uint32_t checksum;
};

RTC_NOINIT_ATTR RetainedCameraMarker retained_marker;

bool initialized = false;
CameraBootDiagnostics boot_diagnostics;

uint32_t MarkerChecksum(uint32_t operation_id, uint32_t stage) {
    return kMarkerMagic ^ operation_id ^ stage ^ kChecksumSalt;
}

bool IsValidMarker() {
    return retained_marker.magic == kMarkerMagic &&
           retained_marker.stage <= static_cast<uint32_t>(CameraDiagnosticStage::kResponseSent) &&
           retained_marker.checksum ==
               MarkerChecksum(retained_marker.operation_id, retained_marker.stage);
}

void WriteMarker(uint32_t operation_id, CameraDiagnosticStage stage) {
    const uint32_t raw_stage = static_cast<uint32_t>(stage);
    retained_marker.checksum = 0;
    retained_marker.magic = kMarkerMagic;
    retained_marker.operation_id = operation_id;
    retained_marker.stage = raw_stage;
    retained_marker.checksum = MarkerChecksum(operation_id, raw_stage);
}

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_UNKNOWN:
        default:
            return "unknown";
    }
}

}  // namespace

void CameraDiagnostics::Initialize() {
    if (initialized) {
        return;
    }

    const bool valid_marker = IsValidMarker();
    const uint32_t previous_operation_id = valid_marker ? retained_marker.operation_id : 0;
    const auto previous_stage =
        valid_marker ? static_cast<CameraDiagnosticStage>(retained_marker.stage)
                     : CameraDiagnosticStage::kNone;

    boot_diagnostics.reset_reason = ResetReasonName(esp_reset_reason());
    boot_diagnostics.interrupted = valid_marker && previous_stage != CameraDiagnosticStage::kNone;
    boot_diagnostics.interrupted_stage = previous_stage;
    boot_diagnostics.interrupted_operation_id = previous_operation_id;
    WriteMarker(previous_operation_id, CameraDiagnosticStage::kNone);
    initialized = true;

    if (boot_diagnostics.interrupted) {
        ESP_LOGW(kTag, "Previous reset=%s interrupted MCP camera operation=%lu stage=%s",
                 boot_diagnostics.reset_reason,
                 static_cast<unsigned long>(boot_diagnostics.interrupted_operation_id),
                 StageName(boot_diagnostics.interrupted_stage));
    } else {
        ESP_LOGI(kTag, "Previous reset=%s; no interrupted MCP camera operation",
                 boot_diagnostics.reset_reason);
    }
}

uint32_t CameraDiagnostics::BeginOperation(CameraDiagnosticStage stage) {
    Initialize();
    const uint32_t next_operation_id = retained_marker.operation_id + 1;
    WriteMarker(next_operation_id, stage);
    return next_operation_id;
}

void CameraDiagnostics::SetStage(CameraDiagnosticStage stage) {
    Initialize();
    WriteMarker(retained_marker.operation_id, stage);
}

void CameraDiagnostics::CompleteOperation() {
    Initialize();
    WriteMarker(retained_marker.operation_id, CameraDiagnosticStage::kNone);
}

CameraBootDiagnostics CameraDiagnostics::GetBootDiagnostics() {
    Initialize();
    return boot_diagnostics;
}

const char* CameraDiagnostics::StageName(CameraDiagnosticStage stage) {
    switch (stage) {
        case CameraDiagnosticStage::kNone:
            return "none";
        case CameraDiagnosticStage::kCapture:
            return "capture";
        case CameraDiagnosticStage::kHttpOpen:
            return "http_open";
        case CameraDiagnosticStage::kUpload:
            return "upload";
        case CameraDiagnosticStage::kWaitStatus:
            return "wait_status";
        case CameraDiagnosticStage::kReadResponse:
            return "read_response";
        case CameraDiagnosticStage::kHttpClose:
            return "http_close";
        case CameraDiagnosticStage::kResultReady:
            return "result_ready";
        case CameraDiagnosticStage::kResultSerialized:
            return "result_serialized";
        case CameraDiagnosticStage::kResponseSent:
            return "response_sent";
        default:
            return "unknown";
    }
}
