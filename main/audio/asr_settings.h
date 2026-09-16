#ifndef ASR_SETTINGS_H
#define ASR_SETTINGS_H

#include <cstddef>
#include <string>

enum class AsrProvider {
    kXiaozhi,
    kGemini,
};

enum class GeminiAsrMode {
    kSmart,
    kVerbatim,
};

struct AsrConfig {
    AsrProvider provider = AsrProvider::kXiaozhi;
    std::string gemini_api_key;
    std::string gemini_language = "vi-VN";
    GeminiAsrMode gemini_mode = GeminiAsrMode::kSmart;
    std::string gemini_vocabulary;

    bool IsGeminiConfigured() const { return !gemini_api_key.empty(); }
};

const char* AsrProviderName(AsrProvider provider);
const char* GeminiAsrModeName(GeminiAsrMode mode);

class AsrSettings {
public:
    static constexpr std::size_t kMaxGeminiVocabularyBytes = 2048;

    static AsrConfig Load();

    // Gemini cannot become the effective provider without a configured API key.
    static bool SetProvider(AsrProvider provider);

    // Blank or masked placeholder values intentionally leave the stored secret unchanged.
    static void UpdateGeminiApiKey(const std::string& api_key);
    static void ClearGeminiApiKey();

    // The initial implementation intentionally supports vi-VN only.
    static bool SetGeminiLanguage(const std::string& language);
    static void SetGeminiMode(GeminiAsrMode mode);
    static bool SetGeminiVocabulary(const std::string& vocabulary);
};

#endif  // ASR_SETTINGS_H
