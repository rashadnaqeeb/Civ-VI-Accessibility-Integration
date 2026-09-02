// Injection of ExposedMembers.CAI into the Lua state captured by the handshake.
#include "cai.h"
#include "hks.h"
#include <string>

bool CaiLooksLikeState(const void* p) { return hks::LooksLikeState(p); }

namespace {
// Unsynchronized on purpose: the handshake runs on every thread that calls a
// time function, but only the game's Lua thread passes the magic time, so
// CaiInject is only ever entered from that one thread.
bool g_injected = false;

// print replacement so Lua output reaches the native log.
int cai_print(lua_State* L) {
    int n = hks::GetTop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += '\t';
        line += hks::ToString(L, *hks::Slot(L, i));
    }
    Log("[Lua] %s", line.c_str());
    return 0;
}

void InstallPrint(lua_State* L) {
    HksObject globals = hks::Globals(L);
    HksObject* saved = hks::Top(L);
    HksObject closure = hks::PushClosure(L, cai_print, "print");
    hks::SetField(L, globals, "print", closure);
    hks::Top(L) = saved;
}
}

void CaiInject(lua_State* L) {
    HksObject globals = hks::Globals(L);
    if (hks::Tag(globals) != hks::TTABLE) { Log("inject: globals is not a table, abort"); return; }
    InstallPrint(L);
    if (g_injected) return;
    HksObject em = hks::GetField(L, globals, "ExposedMembers");
    if (hks::Tag(em) != hks::TTABLE) { Log("inject: ExposedMembers is not a table in this context (tag %llu), waiting for another handshake", (unsigned long long)hks::Tag(em)); return; }
    HksObject* saved = hks::Top(L);
    HksObject cai = hks::PushTable(L, 0, (int)CaiApiCount());
    CaiRegisterApi(L, cai);
    hks::SetField(L, em, "CAI", cai);
    hks::Top(L) = saved;
    g_injected = true;
    Log("inject: ExposedMembers.CAI installed from L=%p globals=%p", (void*)L, (void*)globals.v);
    KeyboardInstall();
    // The first safe moment for network activity: well past the dylib
    // constructor, on an ordinary game thread.
    PlatformStartUpdateCheck();
}
