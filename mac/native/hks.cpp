#include "hks.h"
#include "cai.h"
#include <cstdio>
#include <dlfcn.h>

namespace {
// The game exports, resolved by name at startup. The mangled names encode the
// parameter types but not the return type; the return types below were
// checked against the binary: luaL_checkinteger converts with fcvtzs w0, d0
// and returns an int, hks_obj_getfield returns its HksObject in x0 and x1.
struct GameApi {
    void (*pushnamedcclosure)(lua_State*, lua_CFunction, int, const char*, int);
    void (*createtable)(lua_State*, int, int);
    const char* (*checklstring)(lua_State*, int, unsigned long*);
    double (*checknumber)(lua_State*, int);
    int (*checkinteger)(lua_State*, int);
    HksObject (*getfield)(lua_State*, HksObject, HksObject);
    void (*settable)(lua_State*, const HksObject*, const HksObject*, const HksObject*);
    void (*setmetatable)(lua_State*, const HksObject*, const HksObject*);
    const char* (*tolstring)(lua_State*, HksObject*, unsigned long*);
    HksObject (*newlstringhashed)(lua_State*, const char*, unsigned long, unsigned int);
    void (*growApiStack)(lua_State*, int);
};
GameApi g_api{};

template <class F>
bool Resolve(F& fn, const char* symbol) {
    void* p = dlsym(RTLD_MAIN_ONLY, symbol);
    if (!p) { Log("game does not export %s; staying idle", symbol); return false; }
    fn = reinterpret_cast<F>(p);
    return true;
}

// Slots the API stack must have free beyond what a push needs, so a burst of
// pushes does not grow the stack one slot at a time.
constexpr int kStackHeadroom = 8;
// A C call frame deeper than this is not one of ours; used to reject a
// register that merely looks like a state.
constexpr ptrdiff_t kMaxApiStackSlots = 256;
// See handshake.cpp: no pointer the game holds lies below __PAGEZERO's end.
constexpr uint64_t kPageZeroEnd = 0x100000000ULL;
} // namespace

namespace hks {

bool Bind() {
    return Resolve(g_api.pushnamedcclosure, "_Z21hks_pushnamedcclosureP9lua_StatePFiS0_EiPKci")
        && Resolve(g_api.createtable, "_Z15lua_createtableP9lua_Stateii")
        && Resolve(g_api.checklstring, "_Z17luaL_checklstringP9lua_StateiPm")
        && Resolve(g_api.checknumber, "_Z16luaL_checknumberP9lua_Statei")
        && Resolve(g_api.checkinteger, "_Z17luaL_checkintegerP9lua_Statei")
        && Resolve(g_api.getfield, "_Z16hks_obj_getfieldP9lua_State9HksObjectS1_")
        && Resolve(g_api.settable, "_Z16hks_obj_settableP9lua_StatePK9HksObjectS3_S3_")
        && Resolve(g_api.setmetatable, "_Z20hks_obj_setmetatableP9lua_StatePK9HksObjectS3_")
        && Resolve(g_api.tolstring, "_Z17hks_obj_tolstringP9lua_StateP9HksObjectPm")
        && Resolve(g_api.newlstringhashed, "_Z24hks_obj_newlstringhashedP9lua_StatePKcmj")
        && Resolve(g_api.growApiStack, "_ZN3hks9CallStack12growApiStackEP9lua_Statei");
}

static inline uint32_t Rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
uint32_t Hash(const char* data, size_t len) {
    uint32_t length = len > 31 ? 31 : (uint32_t)len;
    uint32_t a, b, c;
    a = b = c = 0x6b6f7265u + length;
    const uint8_t* k = (const uint8_t*)data;
    auto W = [](const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; };
    while (length > 12) {
        a += W(k); b += W(k + 4); c += W(k + 8);
        a -= c; a ^= Rot(c, 4);  c += b;
        b -= a; b ^= Rot(a, 6);  a += c;
        c -= b; c ^= Rot(b, 8);  b += a;
        a -= c; a ^= Rot(c, 16); c += b;
        b -= a; b ^= Rot(a, 19); a += c;
        c -= b; c ^= Rot(b, 4);  b += a;
        length -= 12; k += 12;
    }
    switch (length) {
    case 12: c += W(k + 8); b += W(k + 4); a += W(k); break;
    case 11: c += (uint32_t)k[10] << 8;  [[fallthrough]];
    case 10: c += (uint32_t)k[9] << 16;  [[fallthrough]];
    case 9:  c += (uint32_t)k[8] << 24;  [[fallthrough]];
    case 8:  b += W(k + 4); a += W(k); break;
    case 7:  b += (uint32_t)k[6] << 8;   [[fallthrough]];
    case 6:  b += (uint32_t)k[5] << 16;  [[fallthrough]];
    case 5:  b += (uint32_t)k[4] << 24;  [[fallthrough]];
    case 4:  a += W(k); break;
    case 3:  a += (uint32_t)k[2] << 8;   [[fallthrough]];
    case 2:  a += (uint32_t)k[1] << 16;  [[fallthrough]];
    case 1:  a += (uint32_t)k[0] << 24;  break;
    case 0:  return c;
    }
    c ^= b; c -= Rot(b, 14);
    a ^= c; a -= Rot(c, 11);
    b ^= a; b -= Rot(a, 25);
    c ^= b; c -= Rot(b, 16);
    a ^= c; a -= Rot(c, 4);
    b ^= a; b -= Rot(a, 14);
    c ^= b; c -= Rot(b, 24);
    return c;
}

bool LooksLikeState(const void* p) {
    const char* c = (const char*)p;
    const HksObject* top = *(const HksObject* const*)(c + OFF_TOP);
    const HksObject* base = *(const HksObject* const*)(c + OFF_BASE);
    const HksObject* g = (const HksObject*)(c + OFF_GLOBALS);
    if (!top || !base || top < base || top - base > kMaxApiStackSlots) return false;
    if (((uintptr_t)top & 0xf) || ((uintptr_t)base & 0xf)) return false;
    if (Tag(*g) != TTABLE || g->v < kPageZeroEnd) return false;
    return true;
}

int GetTop(lua_State* L) { return (int)(Top(L) - Base(L)); }

HksObject* Slot(lua_State* L, int idx) {
    if (idx > 0) return Base(L) + (idx - 1);
    return Top(L) + idx;
}

void EnsureStack(lua_State* L, int n) {
    if (AllocTop(L) - Top(L) < n) g_api.growApiStack(L, n + kStackHeadroom);
}

void Pop(lua_State* L, int n) { Top(L) = Top(L) - n; }

HksObject NewString(lua_State* L, const char* s, size_t len) {
    return g_api.newlstringhashed(L, s, len, Hash(s, len));
}

void Push(lua_State* L, HksObject o) {
    EnsureStack(L, 1);
    *Top(L) = o;
    Top(L) = Top(L) + 1;
}
void PushString(lua_State* L, const char* s, size_t len) { Push(L, NewString(L, s, len)); }
void PushBoolean(lua_State* L, bool b) { Push(L, HksObject{ TBOOLEAN, b ? 1ull : 0ull }); }
void PushNumber(lua_State* L, double d) { HksObject o{ TNUMBER, 0 }; memcpy(&o.v, &d, 8); Push(L, o); }
void PushInteger(lua_State* L, int64_t i) { PushNumber(L, (double)i); }
void PushNil(lua_State* L) { Push(L, HksObject{ TNIL, 0 }); }

HksObject PushTable(lua_State* L, int narr, int nrec) {
    EnsureStack(L, 1);
    g_api.createtable(L, narr, nrec);
    return Top(L)[-1];
}
HksObject PushClosure(lua_State* L, lua_CFunction fn, const char* name) {
    EnsureStack(L, 1);
    g_api.pushnamedcclosure(L, fn, 0, name, 0);
    return Top(L)[-1];
}

const char* CheckString(lua_State* L, int idx, size_t* len) {
    unsigned long l = 0;
    const char* s = g_api.checklstring(L, idx, &l);
    if (len) *len = l;
    return s;
}
double CheckNumber(lua_State* L, int idx) { return g_api.checknumber(L, idx); }
int64_t CheckInteger(lua_State* L, int idx) { return g_api.checkinteger(L, idx); }
bool ToBoolean(lua_State* L, int idx) {
    if (idx > GetTop(L)) return false;
    HksObject* o = Slot(L, idx);
    uint64_t t = Tag(*o);
    if (t == TNIL) return false;
    if (t == TBOOLEAN) return (o->v & 0xff) != 0;
    return true;
}
bool IsNoneOrNil(lua_State* L, int idx) { return idx > GetTop(L) || Tag(*Slot(L, idx)) == TNIL; }

// The key string is a fresh collectable object, so it sits on the stack for
// the duration of the table access in case a metamethod or a collector step
// runs inside it.
HksObject GetField(lua_State* L, HksObject table, const char* key) {
    HksObject k = NewString(L, key);
    Push(L, k);
    HksObject value = g_api.getfield(L, table, k);
    Pop(L, 1);
    return value;
}
void SetField(lua_State* L, HksObject table, const char* key, HksObject value) {
    HksObject k = NewString(L, key);
    Push(L, k);
    g_api.settable(L, &table, &k, &value);
    Pop(L, 1);
}
void SetMetatable(lua_State* L, HksObject table, HksObject mt) { g_api.setmetatable(L, &table, &mt); }

std::string ToString(lua_State* L, HksObject o) {
    char buf[64];
    switch (Tag(o)) {
    case TNIL: return "nil";
    case TBOOLEAN: return (o.v & 0xff) ? "true" : "false";
    case TNUMBER: { double d; memcpy(&d, &o.v, 8); snprintf(buf, sizeof buf, "%.14g", d); return buf; }
    case TSTRING: return std::string(StringData(o), StringLen(o));
    default: {
        HksObject copy = o;
        unsigned long len = 0;
        const char* s = g_api.tolstring(L, &copy, &len);
        if (s) return std::string(s, len);
        const char* kind = "userdata";
        switch (Tag(o)) {
        case TTABLE: kind = "table"; break;
        case TFUNCTION: case TCFUNCTION: case TIFUNCTION: kind = "function"; break;
        case TLIGHTUSERDATA: kind = "lightuserdata"; break;
        case TTHREAD: kind = "thread"; break;
        default: break;
        }
        snprintf(buf, sizeof buf, "<%s: 0x%llx>", kind, (unsigned long long)o.v);
        return buf;
    }
    }
}
}
