// See speechRouter.h.
#include "speechRouter.h"
#include "cai.h"
#include "speech.h"
#include <prism.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
enum class Handler { None, Prism, SystemVoice };

// prism has no feature bit that says "this backend is a screen reader", so
// the auto handler recognizes VoiceOver by its registry name, which starts
// with this prefix ("VoiceOver (macOS)" in prism 0.18).
constexpr const char* kScreenReaderBackendPrefix = "VoiceOver";
// Setting keys in the [Speech] section (settings_CAI.sql).
constexpr const char* kKeyHandler = "SpeechHandler";
constexpr const char* kKeyPrismBackend = "PrismBackend";
constexpr const char* kKeyVoice = "SpeechVoice";
constexpr const char* kKeyRate = "SpeechRate";
constexpr const char* kKeyVolume = "SpeechVolume";
constexpr const char* kDefaultHandler = "systemvoice";   // the streamed system voice; prism is opt-in
constexpr const char* kDefaultVolume = "80";

std::mutex g_mutex;
Handler g_handler = Handler::None;
bool g_activationFailed = false; // the last Activate() started nothing; cleared by a setting change
PrismContext* g_ctx = nullptr;
PrismBackend* g_backend = nullptr;
uint64_t g_features = 0;
bool g_systemInit = false;       // cai_speech_init done (the stream stays alive once started)
std::string g_systemVoice;       // the Spoken Content voice resolved at init

void PrismLog(void*, PrismLogLevel level, const char* source, const char* message) {
    if (level >= PRISM_LOG_LEVEL_WARN) Log("[prism] %s: %s", source ? source : "", message ? message : "");
    else LogDebug("[prism] %s: %s", source ? source : "", message ? message : "");
}
void SpeechLog(cai_speech_log_level level, const char* line) {
    if (level >= CAI_SPEECH_LOG_WARNING) Log("[speech] %s", line);
    else LogDebug("[speech] %s", line);
}

std::string Setting(const char* key, const char* def) { return CaiConfigGet(kSpeechSection, key, def); }

bool IsScreenReaderBackend(PrismBackend* backend) {
    const char* name = prism_backend_name(backend);
    return name && strncmp(name, kScreenReaderBackendPrefix, strlen(kScreenReaderBackendPrefix)) == 0;
}

// ---- system voice ------------------------------------------------------------
bool EnsureSystemVoice() {
    if (g_systemInit) return true;
    cai_speech_set_log(SpeechLog);
    g_systemInit = cai_speech_init();
    Log("system voice init -> %s", g_systemInit ? "ok" : "FAILED");
    if (!g_systemInit) return false;
    char voice[256];
    cai_speech_get_voice(voice, sizeof voice);
    g_systemVoice = voice;
    // Seed the rate so Mod Settings shows the Spoken Content rate instead of a placeholder.
    if (Setting(kKeyRate, "").empty()) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", (int)(cai_speech_get_rate() * 100.0f + 0.5f));
        CaiConfigSet(kSpeechSection, kKeyRate, buf);
    }
    return true;
}

void ApplySystemVoiceSettings() {
    if (!g_systemInit) return;
    std::string volume = Setting(kKeyVolume, kDefaultVolume);
    cai_speech_set_volume((float)atof(volume.c_str()) / 100.0f);
    std::string rate = Setting(kKeyRate, "");
    if (!rate.empty()) cai_speech_set_rate((float)atof(rate.c_str()) / 100.0f);
    std::string voice = Setting(kKeyVoice, "");
    if (voice.empty()) voice = g_systemVoice;   // empty setting: the Spoken Content voice
    if (!voice.empty() && !cai_speech_set_voice(voice.c_str())) Log("system voice: voice %s not installed; keeping the current voice", voice.c_str());
    Log("system voice settings: volume %s, rate %s, voice %s", volume.c_str(), rate.empty() ? "system" : rate.c_str(), voice.empty() ? "default" : voice.c_str());
}

// ---- prism -------------------------------------------------------------------
bool EnsurePrismContext() {
    if (g_ctx) return true;
    static bool logSet = false;
    if (!logSet) { logSet = true; prism_set_log_handler(PrismLogHandler{ PrismLog, nullptr }); }
    PrismConfig cfg = prism_config_init();
    g_ctx = prism_init(&cfg);
    Log("prism %s init -> %s", prism_version_string(), g_ctx ? "ok" : "FAILED");
    return g_ctx != nullptr;
}

void ReleasePrismBackend() {
    if (!g_backend) return;
    (void)prism_backend_stop(g_backend);
    prism_backend_free(g_backend);
    g_backend = nullptr;
    g_features = 0;
}

// Initialize a freshly created backend, tolerating one prism already
// initialized itself. Frees the backend and returns null on failure.
PrismBackend* InitializedBackend(PrismBackend* backend, const char* label) {
    if (!backend) return nullptr;
    PrismError err = prism_backend_initialize(backend);
    if (err == PRISM_OK || err == PRISM_ERROR_ALREADY_INITIALIZED) return backend;
    Log("prism: backend '%s' failed to initialize (%s)", label, prism_error_string(err));
    prism_backend_free(backend);
    return nullptr;
}

// Acquire the configured backend ("auto" or a registry name). With
// requireScreenReader only a VoiceOver backend counts, for the auto handler.
bool AcquirePrismBackend(const std::string& preferred, bool requireScreenReader) {
    if (!EnsurePrismContext()) return false;
    ReleasePrismBackend();
    if (!preferred.empty() && preferred != "auto") {
        PrismBackendId id = prism_registry_id(g_ctx, preferred.c_str());
        if (id == PRISM_BACKEND_INVALID) Log("prism: backend '%s' is not in the registry; using the best available", preferred.c_str());
        else g_backend = InitializedBackend(prism_registry_create(g_ctx, id), preferred.c_str());
    }
    if (!g_backend) g_backend = InitializedBackend(prism_registry_create_best(g_ctx), "best available");
    if (!g_backend) { Log("prism: no backend could be acquired"); return false; }
    const char* name = prism_backend_name(g_backend);
    if (requireScreenReader && !IsScreenReaderBackend(g_backend)) {
        LogDebug("prism: best backend is %s, not a screen reader; auto prefers the system voice", name ? name : "?");
        ReleasePrismBackend();
        return false;
    }
    g_features = prism_backend_get_features(g_backend);
    Log("prism backend: %s (features 0x%llx)", name ? name : "?", (unsigned long long)g_features);
    return true;
}

std::string ActiveNameLocked() {
    if (g_handler == Handler::Prism && g_backend) { const char* n = prism_backend_name(g_backend); return n ? n : "Prism"; }
    if (g_handler == Handler::SystemVoice) return "System voice";
    return "";
}

void ActivateLocked() {
    std::string handler = Setting(kKeyHandler, kDefaultHandler);
    std::string backend = Setting(kKeyPrismBackend, "auto");
    if (g_handler == Handler::SystemVoice) cai_speech_stop();
    ReleasePrismBackend();
    g_handler = Handler::None;

    if (handler == "prism" || handler == "auto") {
        if (AcquirePrismBackend(backend, handler == "auto")) {
            g_handler = Handler::Prism;
        } else if (handler == "prism") {
            Log("speech: prism unavailable; falling back to the system voice");
        }
    }
    if (g_handler == Handler::None) {
        if (EnsureSystemVoice()) g_handler = Handler::SystemVoice;
        else Log("speech: no handler could be started; not retrying until a speech setting changes");
    }
    g_activationFailed = g_handler == Handler::None;
    ApplySystemVoiceSettings();
    Log("speech handler: %s", ActiveNameLocked().c_str());
}
} // namespace

namespace speech {

void Activate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ActivateLocked();
}

void EnsureActive() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_handler != Handler::None || g_activationFailed) return;
    ActivateLocked();
}

void SettingChanged(const char* key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activationFailed = false;
    if (strcmp(key, kKeyHandler) == 0 || strcmp(key, kKeyPrismBackend) == 0 || g_handler == Handler::None) ActivateLocked();
    else ApplySystemVoiceSettings();
}

bool IsActive() { std::lock_guard<std::mutex> lock(g_mutex); return g_handler != Handler::None; }

void Say(const char* text, bool interrupt) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_handler == Handler::Prism && g_backend) {
        // output() speaks and brailles at once; a backend without it reports
        // NOT_IMPLEMENTED, and only then is plain speak() the substitute.
        PrismError err = PRISM_ERROR_NOT_IMPLEMENTED;
        if (g_features & PRISM_BACKEND_SUPPORTS_OUTPUT) err = prism_backend_output(g_backend, text, interrupt);
        if (err == PRISM_ERROR_NOT_IMPLEMENTED) err = prism_backend_speak(g_backend, text, interrupt);
        if (err != PRISM_OK) Log("prism speak failed: %s", prism_error_string(err));
        return;
    }
    if (g_handler == Handler::SystemVoice) cai_speech_say(text, interrupt);
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_handler == Handler::Prism && g_backend) { (void)prism_backend_stop(g_backend); return; }
    if (g_handler == Handler::SystemVoice) cai_speech_stop();
}

bool KeyPressed(bool wait) {
    std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return false;
    if (g_handler == Handler::Prism && g_backend) {
        if (!IsScreenReaderBackend(g_backend)) (void)prism_backend_stop(g_backend);
        return true;
    }
    if (g_handler != Handler::SystemVoice) return true;
    if (wait) { cai_speech_stop(); return true; }
    return cai_speech_try_stop();
}

bool IsSpeaking() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_handler == Handler::Prism && g_backend) {
        bool speaking = false;
        if (prism_backend_is_speaking(g_backend, &speaking) == PRISM_OK) return speaking;
        return false;
    }
    if (g_handler == Handler::SystemVoice) return cai_speech_is_speaking();
    return false;
}

std::string ActiveName() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return ActiveNameLocked();
}

std::string PrismBackendNames() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    if (!EnsurePrismContext()) return out;
    size_t n = prism_registry_count(g_ctx);
    for (size_t i = 0; i < n; ++i) {
        PrismBackendId id = prism_registry_id_at(g_ctx, i);
        const char* name = prism_registry_name(g_ctx, id);
        if (!name) continue;
        // Creating a backend does not initialize it; the runtime-support bit
        // says whether its engine exists on this machine.
        PrismBackend* probe = prism_registry_create(g_ctx, id);
        if (!probe) continue;
        uint64_t features = prism_backend_get_features(probe);
        prism_backend_free(probe);
        if (features & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME) { out += name; out += '\n'; }
    }
    return out;
}

std::string SystemVoices() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    if (!EnsureSystemVoice()) return out;
    int n = cai_speech_voice_count();
    for (int i = 0; i < n; ++i) {
        const char* id = cai_speech_voice_id(i);
        const char* name = cai_speech_voice_name(i);
        const char* lang = cai_speech_voice_language(i);
        if (!id || !name) continue;
        out += id; out += '\t'; out += name; out += '\t'; out += lang ? lang : ""; out += '\n';
    }
    return out;
}

std::string SystemDefaultVoice() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureSystemVoice()) return "";
    return g_systemVoice;
}
}
