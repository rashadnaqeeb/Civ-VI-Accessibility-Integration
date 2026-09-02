// Declarations shared between the units of the CAI macOS dylib. Every unit
// that calls into another includes this instead of re-declaring the function,
// so the format attribute on the log functions is checked everywhere.
#pragma once
#include <cstddef>
#include <string>

struct lua_State;
struct HksObject;

// ---- entry.cpp: logging and state ------------------------------------------
// The log file is ~/Library/Logs/CAI/cai_native.log; every line is stamped
// with the seconds since load and the thread id. Safe from any thread.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Per-call diagnostics, only written when the INI has [Logging] Debug=1.
void LogDebug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void CaiSetDebugLogging(bool on);
// True once the build guard passed; the handshake and the interposers stay
// idle while it is false.
bool CaiIsActive();

// ---- luaapi.cpp: INI configuration and the CAI table ------------------------
// The INI section the speech settings live in (speechRouter.cpp reads it,
// l_SetConfigValue forwards changes to it).
constexpr const char* kSpeechSection = "Speech";
void CaiConfigInit();
std::string CaiConfigGet(const char* section, const char* key, const char* def);
bool CaiConfigSet(const char* section, const char* key, const char* value);
void CaiRegisterApi(lua_State* L, HksObject cai);
size_t CaiApiCount();

// ---- inject.cpp: the handshake target ---------------------------------------
bool CaiLooksLikeState(const void* p);
void CaiInject(lua_State* L);

// ---- platform.mm ------------------------------------------------------------
std::string PlatformClipboardText();
bool PlatformIsGameWindowFocused();
bool PlatformIsCommandDown();
std::string PlatformUserDataDir();
void PlatformBundleVersion(std::string& shortVersion, std::string& build);
void PlatformStartUpdateCheck();
std::string PlatformLatestVersion();

// ---- keyboard.mm ------------------------------------------------------------
// Install the NSEvent monitor that stops speech on every key press and
// queues typed characters. Once per process; safe from any thread.
void KeyboardInstall();
// Whether typed characters are queued for CharInputPoll.
void CharInputEnable(bool enable);
bool CharInputPoll(std::string& out);

// ---- dyld interposing -------------------------------------------------------
// One entry in the __DATA,__interpose section: dyld rebinds every cross-image
// call to `original` so that it lands in `replacement`. The pair is named
// after the replacement so several units can interpose without clashing.
struct DyldInterpose {
    const void* replacement;
    const void* original;
};
#define CAI_INTERPOSE(replacement, original)                                              \
    __attribute__((used)) static DyldInterpose cai_interpose_##replacement                \
        __attribute__((section("__DATA,__interpose"))) = { (const void*)&(replacement), \
                                                           (const void*)&(original) }
