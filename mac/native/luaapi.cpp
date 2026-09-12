// ExposedMembers.CAI implementation for macOS, mirroring the Windows DLL's API
// (extern/civ6-accessibility-lua-integration: luaMethods, luaSpeech, luaAudio,
// luaUpdateManager). Every function uses only the hks binding layer.
#include "cai.h"
#include "hks.h"
#include "audio.h"
#include "SimpleIni.h"
#include "speechRouter.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// --- Config -----------------------------------------------------------------
// Unsynchronized: every reader and writer runs on the game's Lua thread, the
// CAI functions directly and speechRouter.cpp from inside them.
namespace {
struct Config {
    CSimpleIniA ini;
    std::string path;
    bool loaded = false;
    void Load() {
        if (loaded) return;
        loaded = true;
        ini.SetUnicode();
        path = PlatformUserDataDir() + "/civ6-accessibility-integration.ini";
        SI_Error err = ini.LoadFile(path.c_str());
        Log("config: %s (%s)", path.c_str(), err < 0 ? "not found, using defaults" : "loaded");
        // [Logging] Debug=1 turns on the per-call diagnostic lines.
        const char* debug = ini.GetValue("Logging", "Debug", "0");
        CaiSetDebugLogging(debug && atoi(debug) != 0);
    }
    std::string Get(const char* section, const char* key, const char* def) { Load(); const char* v = ini.GetValue(section, key, def); return v ? v : ""; }
    bool Set(const char* section, const char* key, const char* value) {
        Load();
        if (ini.SetValue(section, key, value) < 0) return false;
        return ini.SaveFile(path.c_str()) >= 0;
    }
};
Config& Cfg() { static Config c; return c; }
}
void CaiConfigInit() { Cfg().Load(); }
std::string CaiConfigGet(const char* section, const char* key, const char* def) { return Cfg().Get(section, key, def); }
bool CaiConfigSet(const char* section, const char* key, const char* value) { return Cfg().Set(section, key, value); }

namespace {
// --- Speech (speechRouter.cpp: prism or the streamed system voice) --------------
int l_Output(lua_State* L) {
    const char* text = hks::CheckString(L, 1);
    bool interrupt = hks::ToBoolean(L, 2);
    speech::EnsureActive();
    LogDebug("[say%s] %s", interrupt ? " interrupt" : "", text);
    speech::Say(text, interrupt);
    return 0;
}
int l_Speak(lua_State* L) { return l_Output(L); }
int l_Silence(lua_State* L) { speech::Stop(); return 0; }
// Braille and PreferSapi are Windows screen-reader features with no Mac
// counterpart; they exist so Lua sees the same API surface on both platforms.
int l_Braille(lua_State* L) { hks::CheckString(L, 1); return 0; }
int l_PreferSapi(lua_State* L) { return 0; }
int l_DetectScreenReader(lua_State* L) { hks::PushString(L, speech::ActiveName()); return 1; }
int l_IsSpeaking(lua_State* L) { hks::PushBoolean(L, speech::IsSpeaking()); return 1; }
int l_IsLoaded(lua_State* L) { hks::PushBoolean(L, true); return 1; }

// --- Config -----------------------------------------------------------------
int l_GetConfigValue(lua_State* L) {
    const char* section = hks::CheckString(L, 1);
    const char* key = hks::CheckString(L, 2);
    const char* def = hks::CheckString(L, 3);
    hks::PushString(L, Cfg().Get(section, key, def));
    return 1;
}
int l_SetConfigValue(lua_State* L) {
    const char* section = hks::CheckString(L, 1);
    const char* key = hks::CheckString(L, 2);
    const char* value = hks::CheckString(L, 3);
    bool ok = Cfg().Set(section, key, value);
    // Speech settings changed from Mod Settings take effect at once.
    if (ok && strcmp(section, kSpeechSection) == 0) speech::SettingChanged(key);
    hks::PushBoolean(L, ok);
    return 1;
}
// For the Speech section of Mod Settings.
int l_GetSpeechVoices(lua_State* L) { hks::PushString(L, speech::SystemVoices()); return 1; }
int l_GetSpeechSystemVoice(lua_State* L) { hks::PushString(L, speech::SystemDefaultVoice()); return 1; }
int l_GetPrismBackends(lua_State* L) { hks::PushString(L, speech::PrismBackendNames()); return 1; }

// --- Misc -------------------------------------------------------------------
int l_ResetInputBindings(lua_State* L) {
    std::string p = PlatformUserDataDir() + "/Firaxis Games/Sid Meier's Civilization VI/InputSettings.json";
    bool ok = unlink(p.c_str()) == 0;
    Log("ResetInputBindings: %s -> %s", p.c_str(), ok ? "removed" : "not removed");
    hks::PushBoolean(L, ok);
    return 1;
}
int l_GetClipboardText(lua_State* L) { hks::PushString(L, PlatformClipboardText()); return 1; }
int l_IsGameWindowFocused(lua_State* L) { hks::PushBoolean(L, PlatformIsGameWindowFocused()); return 1; }
int l_IsCommandDown(lua_State* L) { hks::PushBoolean(L, PlatformIsCommandDown()); return 1; }
int l_GetLatestVersion(lua_State* L) { hks::PushString(L, PlatformLatestVersion()); return 1; }
// True while an input method holds marked text in the game window, sampled by
// the key monitor around each key (keyboard.mm).
int l_IsImeComposing(lua_State* L) { hks::PushBoolean(L, KeyboardIsComposing()); return 1; }
// On Mac the handler function is not stored: Lua polls PollCharInput() each frame instead.
int l_RegisterGlobalCharInputHandler(lua_State* L) { CharInputEnable(true); return 0; }
int l_UnregisterGlobalCharInputHandler(lua_State* L) { CharInputEnable(false); return 0; }
// Returns the character and the CAI.GetTime() reading at its key-down, or nil.
int l_PollCharInput(lua_State* L) {
    std::string s;
    double time = 0;
    if (!CharInputPoll(s, time)) { hks::PushNil(L); return 1; }
    hks::PushString(L, s);
    hks::PushNumber(L, time);
    return 2;
}

// --- Audio (audio.h, miniaudio) ---------------------------------------------
// Started on the first call that needs it (the Lua audio manager loads its
// sounds at init), never from the dylib constructor.
bool g_audioTried = false;
void EnsureAudio() {
    if (g_audioTried) return;
    g_audioTried = true;
    if (!audio::Initialize()) Log("audio: initialization failed; sounds are off");
}
audio::Handle SoundArg(lua_State* L) { return (audio::Handle)hks::CheckInteger(L, 1); }
float FloatArg(lua_State* L, int idx) { return (float)hks::CheckNumber(L, idx); }

int l_LoadSound(lua_State* L) {
    const char* path = hks::CheckString(L, 1);
    EnsureAudio();
    auto h = audio::LoadSound(path);
    if (!h) { hks::PushNil(L); return 1; }
    hks::PushInteger(L, *h);
    return 1;
}
int l_DestroySound(lua_State* L) { hks::PushBoolean(L, audio::DestroySound(SoundArg(L))); return 1; }
int l_PlaySound(lua_State* L) { audio::Play(SoundArg(L)); return 0; }
int l_PauseSound(lua_State* L) { audio::Pause(SoundArg(L)); return 0; }
int l_StopSound(lua_State* L) { audio::Stop(SoundArg(L)); return 0; }
int l_SetSoundVolume(lua_State* L) { audio::SetVolume(SoundArg(L), FloatArg(L, 2)); return 0; }
int l_GetSoundVolume(lua_State* L) { hks::PushNumber(L, audio::GetVolume(SoundArg(L))); return 1; }
int l_SetSoundLooping(lua_State* L) { audio::SetLooping(SoundArg(L), hks::ToBoolean(L, 2)); return 0; }
int l_IsSoundLooping(lua_State* L) { hks::PushBoolean(L, audio::IsLooping(SoundArg(L))); return 1; }
int l_SetSoundPitch(lua_State* L) { audio::SetPitch(SoundArg(L), FloatArg(L, 2)); return 0; }
int l_GetSoundPitch(lua_State* L) { hks::PushNumber(L, audio::GetPitch(SoundArg(L))); return 1; }
int l_SetSoundPan(lua_State* L) { audio::SetPan(SoundArg(L), FloatArg(L, 2)); return 0; }
int l_GetSoundPan(lua_State* L) { hks::PushNumber(L, audio::GetPan(SoundArg(L))); return 1; }
int l_SetSoundPosition(lua_State* L) { audio::SetPosition(SoundArg(L), FloatArg(L, 2), FloatArg(L, 3), FloatArg(L, 4)); return 0; }
int l_GetSoundPosition(lua_State* L) {
    float x, y, z;
    audio::GetPosition(SoundArg(L), x, y, z);
    hks::PushNumber(L, x); hks::PushNumber(L, y); hks::PushNumber(L, z);
    return 3;
}
int l_SetSoundDirection(lua_State* L) { audio::SetDirection(SoundArg(L), FloatArg(L, 2), FloatArg(L, 3), FloatArg(L, 4)); return 0; }
int l_SetSoundVelocity(lua_State* L) { audio::SetVelocity(SoundArg(L), FloatArg(L, 2), FloatArg(L, 3), FloatArg(L, 4)); return 0; }
int l_SetSoundSpatializationEnabled(lua_State* L) { audio::SetSpatializationEnabled(SoundArg(L), hks::ToBoolean(L, 2)); return 0; }
int l_IsSoundSpatializationEnabled(lua_State* L) { hks::PushBoolean(L, audio::IsSpatializationEnabled(SoundArg(L))); return 1; }
int l_SetSoundMinDistance(lua_State* L) { audio::SetMinDistance(SoundArg(L), FloatArg(L, 2)); return 0; }
int l_SetSoundMaxDistance(lua_State* L) { audio::SetMaxDistance(SoundArg(L), FloatArg(L, 2)); return 0; }
int l_SetSoundAttenuationModel(lua_State* L) {
    int64_t model = hks::CheckInteger(L, 2);
    if (model < 0 || model > (int64_t)audio::AttenuationModel::Exponential) {
        Log("SetSoundAttenuationModel: %lld is not an attenuation model; ignored", (long long)model);
        return 0;
    }
    audio::SetAttenuationModel(SoundArg(L), (audio::AttenuationModel)model);
    return 0;
}
int l_IsSoundPlaying(lua_State* L) { hks::PushBoolean(L, audio::IsPlaying(SoundArg(L))); return 1; }
int l_SetListenerPosition(lua_State* L) { audio::SetListenerPosition(FloatArg(L, 1), FloatArg(L, 2), FloatArg(L, 3)); return 0; }
int l_SetListenerDirection(lua_State* L) { audio::SetListenerDirection(FloatArg(L, 1), FloatArg(L, 2), FloatArg(L, 3)); return 0; }
int l_SetListenerUp(lua_State* L) { audio::SetListenerUp(FloatArg(L, 1), FloatArg(L, 2), FloatArg(L, 3)); return 0; }
int l_SetListenerVelocity(lua_State* L) { audio::SetListenerVelocity(FloatArg(L, 1), FloatArg(L, 2), FloatArg(L, 3)); return 0; }
int l_SetMasterVolume(lua_State* L) { EnsureAudio(); audio::SetMasterVolume(FloatArg(L, 1)); return 0; }
int l_GetMasterVolume(lua_State* L) { hks::PushNumber(L, audio::GetMasterVolume()); return 1; }
int l_AudioUpdate(lua_State* L) { EnsureAudio(); audio::Update(); return 0; }
// Seconds from a monotonic clock, as a double. Automation.GetTime() on the
// Aspyr build advances only once per second, so every CAI timer that needs
// sub-second resolution reads this through GetMonotonicTime() in caiUtils.lua.
int l_GetTime(lua_State* L) {
    using namespace std::chrono;
    hks::PushNumber(L, duration<double>(steady_clock::now().time_since_epoch()).count());
    return 1;
}

struct Entry { const char* name; lua_CFunction fn; };
const Entry kApi[] = {
    { "ResetInputBindings", l_ResetInputBindings },
    { "RegisterGlobalCharInputHandler", l_RegisterGlobalCharInputHandler },
    { "UnregisterGlobalCharInputHandler", l_UnregisterGlobalCharInputHandler },
    { "IsImeComposing", l_IsImeComposing },
    { "PollCharInput", l_PollCharInput },
    { "GetClipboardText", l_GetClipboardText },
    { "GetConfigValue", l_GetConfigValue },
    { "SetConfigValue", l_SetConfigValue },
    { "IsGameWindowFocused", l_IsGameWindowFocused },
    { "IsCommandDown", l_IsCommandDown },
    { "Output", l_Output }, { "Speak", l_Speak }, { "Braille", l_Braille }, { "Silence", l_Silence },
    { "PreferSapi", l_PreferSapi }, { "DetectScreenReader", l_DetectScreenReader },
    { "IsSpeaking", l_IsSpeaking }, { "IsLoaded", l_IsLoaded },
    { "GetLatestVersion", l_GetLatestVersion },
    { "GetSpeechVoices", l_GetSpeechVoices }, { "GetSpeechSystemVoice", l_GetSpeechSystemVoice }, { "GetPrismBackends", l_GetPrismBackends },
    { "LoadSound", l_LoadSound }, { "DestroySound", l_DestroySound },
    { "PlaySound", l_PlaySound }, { "PauseSound", l_PauseSound }, { "StopSound", l_StopSound },
    { "SetSoundVolume", l_SetSoundVolume }, { "GetSoundVolume", l_GetSoundVolume },
    { "SetSoundLooping", l_SetSoundLooping }, { "IsSoundLooping", l_IsSoundLooping },
    { "SetSoundPitch", l_SetSoundPitch }, { "GetSoundPitch", l_GetSoundPitch },
    { "SetSoundPan", l_SetSoundPan }, { "GetSoundPan", l_GetSoundPan },
    { "SetSoundPosition", l_SetSoundPosition }, { "GetSoundPosition", l_GetSoundPosition },
    { "SetSoundDirection", l_SetSoundDirection }, { "SetSoundVelocity", l_SetSoundVelocity },
    { "SetSoundSpatializationEnabled", l_SetSoundSpatializationEnabled }, { "IsSoundSpatializationEnabled", l_IsSoundSpatializationEnabled },
    { "SetSoundMinDistance", l_SetSoundMinDistance }, { "SetSoundMaxDistance", l_SetSoundMaxDistance }, { "SetSoundAttenuationModel", l_SetSoundAttenuationModel },
    { "IsSoundPlaying", l_IsSoundPlaying },
    { "SetListenerPosition", l_SetListenerPosition }, { "SetListenerDirection", l_SetListenerDirection }, { "SetListenerUp", l_SetListenerUp }, { "SetListenerVelocity", l_SetListenerVelocity },
    { "SetMasterVolume", l_SetMasterVolume }, { "GetMasterVolume", l_GetMasterVolume },
    { "AudioUpdate", l_AudioUpdate },
    { "GetTime", l_GetTime },
};
}

size_t CaiApiCount() { return sizeof kApi / sizeof kApi[0]; }

void CaiRegisterApi(lua_State* L, HksObject cai) {
    HksObject* saved = hks::Top(L);
    for (const Entry& e : kApi) {
        HksObject closure = hks::PushClosure(L, e.fn, e.name);
        hks::SetField(L, cai, e.name, closure);
        hks::Pop(L, 1);
    }
    hks::Top(L) = saved;
    Log("inject: registered %zu CAI functions", CaiApiCount());
}
