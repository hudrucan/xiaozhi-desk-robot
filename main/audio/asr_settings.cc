#include "asr_settings.h"

#include <cctype>
#include <string_view>

#include "settings.h"

namespace {

constexpr char kNamespace[] = "gem_asr";
constexpr char kProviderKey[] = "provider";
constexpr char kApiKeyKey[] = "api_key";
constexpr char kLanguageKey[] = "lang";
constexpr char kModeKey[] = "mode";
constexpr char kVocabularyKey[] = "vocab";

constexpr char kXiaozhiProvider[] = "xiaozhi";
constexpr char kGeminiProvider[] = "gemini";
constexpr char kDefaultLanguage[] = "vi-VN";
constexpr char kSmartMode[] = "SMART";
constexpr char kVerbatimMode[] = "VERBATIM";

constexpr size_t kNvsNameMaxLength = 15;
static_assert(std::string_view(kNamespace).size() <= kNvsNameMaxLength);
static_assert(std::string_view(kProviderKey).size() <= kNvsNameMaxLength);
static_assert(std::string_view(kApiKeyKey).size() <= kNvsNameMaxLength);
static_assert(std::string_view(kLanguageKey).size() <= kNvsNameMaxLength);
static_assert(std::string_view(kModeKey).size() <= kNvsNameMaxLength);
static_assert(std::string_view(kVocabularyKey).size() <= kNvsNameMaxLength);

std::string_view TrimWhitespace(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool IsBlankOrMasked(std::string_view value) {
    if (value.empty()) {
        return true;
    }

    constexpr std::string_view kBullet = "\xE2\x80\xA2";
    return value.front() == '*' || value.starts_with(kBullet);
}

AsrProvider ParseProvider(const std::string& value) {
    return value == kGeminiProvider ? AsrProvider::kGemini : AsrProvider::kXiaozhi;
}

GeminiAsrMode ParseMode(const std::string& value) {
    return value == kVerbatimMode ? GeminiAsrMode::kVerbatim : GeminiAsrMode::kSmart;
}

}  // namespace

const char* AsrProviderName(AsrProvider provider) {
    return provider == AsrProvider::kGemini ? kGeminiProvider : kXiaozhiProvider;
}

const char* GeminiAsrModeName(GeminiAsrMode mode) {
    return mode == GeminiAsrMode::kVerbatim ? kVerbatimMode : kSmartMode;
}

AsrConfig AsrSettings::Load() {
    Settings settings(kNamespace);
    AsrConfig config;
    config.gemini_api_key = settings.GetString(kApiKeyKey);
    config.gemini_language = settings.GetString(kLanguageKey, kDefaultLanguage);
    if (config.gemini_language != kDefaultLanguage) {
        config.gemini_language = kDefaultLanguage;
    }
    config.gemini_mode = ParseMode(settings.GetString(kModeKey, kSmartMode));
    config.gemini_vocabulary = settings.GetString(kVocabularyKey);
    config.provider = ParseProvider(settings.GetString(kProviderKey, kXiaozhiProvider));

    // A stale/corrupt Gemini selection must never make an existing device unusable.
    if (config.provider == AsrProvider::kGemini && !config.IsGeminiConfigured()) {
        config.provider = AsrProvider::kXiaozhi;
    }
    return config;
}

bool AsrSettings::SetProvider(AsrProvider provider) {
    if (provider == AsrProvider::kGemini && !Load().IsGeminiConfigured()) {
        return false;
    }
    Settings settings(kNamespace, true);
    settings.SetString(kProviderKey, AsrProviderName(provider));
    return true;
}

void AsrSettings::UpdateGeminiApiKey(const std::string& api_key) {
    const std::string_view trimmed = TrimWhitespace(api_key);
    if (IsBlankOrMasked(trimmed)) {
        return;
    }
    Settings settings(kNamespace, true);
    settings.SetString(kApiKeyKey, std::string(trimmed));
}

void AsrSettings::ClearGeminiApiKey() {
    Settings settings(kNamespace, true);
    settings.SetString(kApiKeyKey, "");
    if (settings.GetString(kProviderKey, kXiaozhiProvider) == kGeminiProvider) {
        settings.SetString(kProviderKey, kXiaozhiProvider);
    }
}

bool AsrSettings::SetGeminiLanguage(const std::string& language) {
    if (language != kDefaultLanguage) {
        return false;
    }
    Settings settings(kNamespace, true);
    settings.SetString(kLanguageKey, language);
    return true;
}

void AsrSettings::SetGeminiMode(GeminiAsrMode mode) {
    Settings settings(kNamespace, true);
    settings.SetString(kModeKey, GeminiAsrModeName(mode));
}

bool AsrSettings::SetGeminiVocabulary(const std::string& vocabulary) {
    if (vocabulary.size() > kMaxGeminiVocabularyBytes) {
        return false;
    }
    Settings settings(kNamespace, true);
    settings.SetString(kVocabularyKey, vocabulary);
    return true;
}
