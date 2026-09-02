// CAI macOS dylib entry: logging, the build guard, and the capture of Lua
// runtime errors. Loaded into Civ6_Exe_Child through DYLD_INSERT_LIBRARIES;
// stays idle in the Civ6_Exe launcher and in any other build than 1.4.6.
// The Lua state itself is learned by the os.date handshake in handshake.cpp.
//
// Build: mac/dev/build.sh (CMake, see CMakeLists.txt). Run: mac/dev/run.sh.
// Log: ~/Library/Logs/CAI/cai_native.log
// (the previous run is kept as cai_native.log.1).

#include "cai.h"
#include "hks.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

// The one game build whose Havok Script layout (mac/README.md) this dylib
// was verified against. install.sh carries the same build number for its
// pre-flight warning; package.sh checks that the two agree.
constexpr const char* kSupportedVersion = "1.4.6";
constexpr const char* kSupportedBuild = "376653.178";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static uint64_t g_t0 = 0;
static std::atomic<bool> g_debug{false};
static std::atomic<bool> g_active{false};

static double NowSeconds() {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time() - g_t0;
    return (double)t * tb.numer / tb.denom / 1e9;
}

static void LogV(const char* fmt, va_list ap) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    fprintf(g_log, "[%9.3f] [tid %llu] ", NowSeconds(), (unsigned long long)tid);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// Off unless the INI has [Logging] Debug=1; the check is a relaxed atomic load.
void LogDebug(const char* fmt, ...) {
    if (!g_debug.load(std::memory_order_relaxed)) return;
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void CaiSetDebugLogging(bool on) { g_debug.store(on); }
bool CaiIsActive() { return g_active.load(std::memory_order_relaxed); }

static std::string LogDir() {
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/Library/Logs/CAI" : std::string("/tmp/CAI");
}

// ---------------------------------------------------------------------------
// Lua error capture: the exe formats "Runtime Error: %s" and the traceback
// frames through libc, so interposing the formatters yields a Lua.log. Every
// snprintf in the process passes through here, so the checks are ordered
// cheapest first and nothing is scanned while the dylib is idle.
// ---------------------------------------------------------------------------
// Longest error text copied into the log per call.
constexpr int kLuaErrorMaxChars = 1500;

static bool Interesting(const char* fmt, const char* out) {
    return (fmt && strstr(fmt, "Runtime Error")) ||
           (out && (strstr(out, ".lua:") || strstr(out, "Runtime Error") || strstr(out, "attempt to")));
}
// vsnprintf only writes and terminates the buffer when n > 0; with n == 0 the
// call is a size query and buf may be anything.
static void CaptureLuaError(const char* fmt, const char* buf, size_t n, int written) {
    if (written <= 0 || n == 0 || !g_active.load(std::memory_order_relaxed)) return;
    if (Interesting(fmt, buf)) Log("[Lua error] %.*s", kLuaErrorMaxChars, buf);
}

static int my_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    int r = vsnprintf(buf, n, fmt, ap);
    CaptureLuaError(fmt, buf, n, r);
    return r;
}
CAI_INTERPOSE(my_vsnprintf, vsnprintf);

extern "C" int __vsnprintf_chk(char*, size_t, int, size_t, const char*, va_list);
static int my_vsnprintf_chk(char* buf, size_t n, int flag, size_t sz, const char* fmt, va_list ap) {
    int r = __vsnprintf_chk(buf, n, flag, sz, fmt, ap);
    CaptureLuaError(fmt, buf, n, r);
    return r;
}
CAI_INTERPOSE(my_vsnprintf_chk, __vsnprintf_chk);

static int my_snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    CaptureLuaError(fmt, buf, n, r);
    return r;
}
CAI_INTERPOSE(my_snprintf, snprintf);

extern "C" int __snprintf_chk(char*, size_t, int, size_t, const char*, ...);
static int my_snprintf_chk(char* buf, size_t n, int flag, size_t sz, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __vsnprintf_chk(buf, n, flag, sz, fmt, ap);
    va_end(ap);
    CaptureLuaError(fmt, buf, n, r);
    return r;
}
CAI_INTERPOSE(my_snprintf_chk, __snprintf_chk);

// ---------------------------------------------------------------------------
// Build guard: the Lua binding relies on exported symbols and on the
// lua_State layout of build 1.4.6. Anything else stays idle.
// ---------------------------------------------------------------------------
static bool CheckGameBuild() {
    std::string version, build;
    PlatformBundleVersion(version, build);
    Log("game bundle version %s build %s", version.c_str(), build.c_str());
    if (version != kSupportedVersion || build != kSupportedBuild) {
        Log("unsupported game build (this dylib supports %s build %s); staying idle", kSupportedVersion, kSupportedBuild);
        return false;
    }
    return hks::Bind();
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
__attribute__((constructor))
static void CaiInit() {
    g_t0 = mach_absolute_time();
    char exe[MAXPATHLEN] = "";
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) return;
    // The launcher (Civ6_Exe) loads the dylib too when DYLD_INSERT_LIBRARIES is
    // set through Steam; it must not touch the log or anything else.
    if (strstr(exe, "Civ6_Exe_Child") == nullptr) return;
    std::string dir = LogDir();
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/cai_native.log";
    rename(path.c_str(), (path + ".1").c_str());
    g_log = fopen(path.c_str(), "w");
    Log("CAI dylib loaded in pid %d exe=%s", getpid(), exe);
    if (!CheckGameBuild()) return;
    CaiConfigInit();
    g_active.store(true);
    // Nothing else starts here: this runs under dyld's constructor lock before
    // any static initializer of the game. The sound engine, speech and the
    // release check all start from the first Lua call that needs them.
    Log("waiting for the Lua handshake");
}

__attribute__((destructor))
static void CaiShutdown() {
    Log("CAI dylib unloading");
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) fclose(g_log);
    g_log = nullptr;
}
