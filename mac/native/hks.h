// Minimal Havok Script binding layer for the Mac build of Civ VI (1.4.6, arm64).
// Uses only symbols exported by Civ6_Exe_Child, resolved with dlsym in Bind(),
// plus the lua_State layout documented in mac/README.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

struct lua_State;
// A Havok Script value: a type tag in the low bits of the first word and the
// payload in the second. The name and the 16-byte layout are part of the
// mangled names of the game exports Bind() resolves, so neither may change.
struct HksObject { uint64_t t; uint64_t v; };
typedef int (*lua_CFunction)(lua_State*);

namespace hks {
enum Type : uint64_t { TNIL = 0, TBOOLEAN = 1, TLIGHTUSERDATA = 2, TNUMBER = 3, TSTRING = 4, TTABLE = 5, TFUNCTION = 6, TUSERDATA = 7, TTHREAD = 8, TIFUNCTION = 9, TCFUNCTION = 10 };

// lua_State layout, offsets from the state pointer (mac/README.md, "Havok
// Script on this binary").
constexpr size_t OFF_TOP = 0x48, OFF_BASE = 0x50, OFF_ALLOCTOP = 0x58, OFF_GLOBALS = 0x70, OFF_NEXTSTATE = 0xb0;
// TString layout, offsets from the string object: the length word carries two
// flag bits at the top; the characters follow the 32-bit hash.
constexpr size_t OFF_TSTRING_LEN = 0x08, OFF_TSTRING_DATA = 0x14;
constexpr uint64_t TSTRING_LEN_MASK = 0x3fffffffffffffffULL;

inline HksObject*& Top(lua_State* L) { return *(HksObject**)((char*)L + OFF_TOP); }
inline HksObject* Base(lua_State* L) { return *(HksObject**)((char*)L + OFF_BASE); }
inline HksObject* AllocTop(lua_State* L) { return *(HksObject**)((char*)L + OFF_ALLOCTOP); }
inline HksObject Globals(lua_State* L) { return *(HksObject*)((char*)L + OFF_GLOBALS); }
inline uint64_t Tag(const HksObject& o) { return o.t & 0xf; }
inline const char* StringData(const HksObject& o) { return (const char*)o.v + OFF_TSTRING_DATA; }
inline size_t StringLen(const HksObject& o) { return *(const uint64_t*)(o.v + OFF_TSTRING_LEN) & TSTRING_LEN_MASK; }

// Resolve the game exports with dlsym. False, with a log line naming the
// symbol, if one is missing; nothing else in this namespace may be called then.
bool Bind();

uint32_t Hash(const char* data, size_t len);
bool LooksLikeState(const void* p);

int GetTop(lua_State* L);
HksObject* Slot(lua_State* L, int idx);   // 1-based positive or negative index into the C call frame
void EnsureStack(lua_State* L, int n);
void Pop(lua_State* L, int n);

HksObject NewString(lua_State* L, const char* s, size_t len);
inline HksObject NewString(lua_State* L, const char* s) { return NewString(L, s, strlen(s)); }
void Push(lua_State* L, HksObject o);
void PushString(lua_State* L, const char* s, size_t len);
inline void PushString(lua_State* L, const char* s) { PushString(L, s, strlen(s)); }
inline void PushString(lua_State* L, const std::string& s) { PushString(L, s.data(), s.size()); }
void PushBoolean(lua_State* L, bool b);
void PushNumber(lua_State* L, double d);
void PushInteger(lua_State* L, int64_t i);
void PushNil(lua_State* L);
HksObject PushTable(lua_State* L, int narr = 0, int nrec = 0);      // leaves it on the stack, returns the object
HksObject PushClosure(lua_State* L, lua_CFunction fn, const char* name); // leaves it on the stack

const char* CheckString(lua_State* L, int idx, size_t* len = nullptr);
double CheckNumber(lua_State* L, int idx);
int64_t CheckInteger(lua_State* L, int idx);
bool ToBoolean(lua_State* L, int idx);
bool IsNoneOrNil(lua_State* L, int idx);

HksObject GetField(lua_State* L, HksObject table, const char* key);
void SetField(lua_State* L, HksObject table, const char* key, HksObject value);
void SetMetatable(lua_State* L, HksObject table, HksObject mt);
std::string ToString(lua_State* L, HksObject o);  // tostring-like for logging
}
