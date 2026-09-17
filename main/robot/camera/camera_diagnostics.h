#pragma once

#include <cstdint>

enum class CameraDiagnosticStage : uint32_t {
    kNone = 0,
    kCapture,
    kHttpOpen,
    kUpload,
    kWaitStatus,
    kReadResponse,
    kHttpClose,
    kResultReady,
    kResultSerialized,
    kResponseSent,
};

struct CameraBootDiagnostics {
    const char* reset_reason = "unknown";
    bool interrupted = false;
    CameraDiagnosticStage interrupted_stage = CameraDiagnosticStage::kNone;
    uint32_t interrupted_operation_id = 0;
};

class CameraDiagnostics {
public:
    static void Initialize();
    static uint32_t BeginOperation(CameraDiagnosticStage stage);
    static void SetStage(CameraDiagnosticStage stage);
    static void CompleteOperation();

    static CameraBootDiagnostics GetBootDiagnostics();
    static const char* StageName(CameraDiagnosticStage stage);
};
