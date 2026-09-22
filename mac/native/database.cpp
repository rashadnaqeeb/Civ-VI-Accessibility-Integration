// CAI.OpenDatabase, CAI.Query and CAI.CloseDatabase for macOS, mirroring the
// Windows DLL's SQLite bridge (extern/civ6-accessibility-lua-integration:
// luaDatabase, database, databaseManager). Links the system libsqlite3.
// Unsynchronized: every call runs on the game's Lua thread.
#include "cai.h"
#include "hks.h"
#include <sqlite3.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// Handles count up from 1 and are never reused, as on Windows.
std::unordered_map<uint32_t, sqlite3*> g_databases;
uint32_t g_nextHandle = 1;

// A bound parameter. Lua numbers bind as doubles and booleans as 0 or 1, the
// same conversions the Windows DLL makes.
struct Param {
    enum Kind { Integer, Real, Text } kind;
    int64_t i = 0;
    double d = 0;
    std::string s;
};

// Reads params[1], params[2], ... up to the first nil. A missing or non-table
// argument means no parameters.
std::vector<Param> ReadParams(lua_State* L, int idx) {
    std::vector<Param> params;
    if (hks::IsNoneOrNil(L, idx)) return params;
    HksObject table = *hks::Slot(L, idx);
    if (hks::Tag(table) != hks::TTABLE) return params;
    for (int n = 1;; ++n) {
        HksObject v = hks::RawGetI(L, table, n);
        switch (hks::Tag(v)) {
        case hks::TNIL: return params;
        case hks::TNUMBER: params.push_back({ Param::Real, 0, hks::NumberValue(v), {} }); break;
        case hks::TBOOLEAN: params.push_back({ Param::Integer, (v.v & 0xff) ? 1 : 0, 0, {} }); break;
        case hks::TSTRING: params.push_back({ Param::Text, 0, 0, std::string(hks::StringData(v), hks::StringLen(v)) }); break;
        default: params.push_back({ Param::Text, 0, 0, hks::ToString(L, v) }); break;
        }
    }
}

int Bind(sqlite3_stmt* stmt, int index, const Param& p) {
    switch (p.kind) {
    case Param::Integer: return sqlite3_bind_int64(stmt, index, p.i);
    case Param::Real: return sqlite3_bind_double(stmt, index, p.d);
    case Param::Text: return sqlite3_bind_text(stmt, index, p.s.data(), (int)p.s.size(), SQLITE_TRANSIENT);
    }
    return SQLITE_MISUSE;
}

// Pushes nil and the message, the failure return of every function here.
int Fail(lua_State* L, const std::string& message) {
    LogDebug("database: %s", message.c_str());
    hks::PushNil(L);
    hks::PushString(L, message);
    return 2;
}

// Sets table[key] to a fresh string, keeping the string on the stack until
// the table holds it.
void SetStringField(lua_State* L, HksObject table, const char* key, const char* s, size_t len) {
    hks::PushString(L, s, len);
    hks::SetField(L, table, key, hks::Slot(L, -1)[0]);
    hks::Pop(L, 1);
}
}

int CaiDbOpenDatabase(lua_State* L) {
    const char* path = hks::CheckString(L, 1);
    sqlite3* db = nullptr;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "unable to allocate database handle";
        sqlite3_close(db);
        return Fail(L, error);
    }
    uint32_t handle = g_nextHandle++;
    g_databases[handle] = db;
    LogDebug("database: opened %u: %s", handle, path);
    hks::PushInteger(L, handle);
    return 1;
}

int CaiDbCloseDatabase(lua_State* L) {
    auto it = g_databases.find((uint32_t)hks::CheckInteger(L, 1));
    bool found = it != g_databases.end();
    if (found) {
        sqlite3_close_v2(it->second);
        g_databases.erase(it);
    }
    hks::PushBoolean(L, found);
    return 1;
}

// Runs one statement. The result table holds the rows at 1..n, each keyed by
// column name (NULL columns absent), plus columns, changed and lastInsertRowId.
int CaiDbQuery(lua_State* L) {
    auto it = g_databases.find((uint32_t)hks::CheckInteger(L, 1));
    size_t sqlLen = 0;
    const char* sqlArg = hks::CheckString(L, 2, &sqlLen);
    std::string sql(sqlArg, sqlLen);
    std::vector<Param> params = ReadParams(L, 3);
    if (it == g_databases.end()) return Fail(L, "invalid database handle");
    sqlite3* db = it->second;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), (int)sql.size(), &stmt, nullptr) != SQLITE_OK)
        return Fail(L, sqlite3_errmsg(db));
    for (size_t i = 0; i < params.size(); ++i) {
        if (Bind(stmt, (int)i + 1, params[i]) != SQLITE_OK) {
            std::string error = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return Fail(L, error);
        }
    }

    // The result table is built as the rows are stepped, and stays on the
    // stack throughout so the collector sees it.
    int columnCount = sqlite3_column_count(stmt);
    std::vector<std::string> columns;
    HksObject result = hks::PushTable(L, 0, 3);
    HksObject columnTable = hks::PushTable(L, columnCount, 0);
    for (int c = 0; c < columnCount; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        columns.emplace_back(name ? name : "");
        hks::PushString(L, columns.back());
        hks::SetIndex(L, columnTable, c + 1, hks::Slot(L, -1)[0]);
        hks::Pop(L, 1);
    }
    hks::SetField(L, result, "columns", columnTable);
    hks::Pop(L, 1);

    int rc;
    int rowCount = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        HksObject row = hks::PushTable(L, 0, columnCount);
        for (int c = 0; c < columnCount; ++c) {
            const char* key = columns[c].c_str();
            switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER: hks::SetField(L, row, key, hks::Number((double)sqlite3_column_int64(stmt, c))); break;
            case SQLITE_FLOAT: hks::SetField(L, row, key, hks::Number(sqlite3_column_double(stmt, c))); break;
            case SQLITE_TEXT:
                SetStringField(L, row, key, (const char*)sqlite3_column_text(stmt, c), (size_t)sqlite3_column_bytes(stmt, c));
                break;
            case SQLITE_BLOB: {
                // An empty blob comes back as a null pointer.
                const void* data = sqlite3_column_blob(stmt, c);
                SetStringField(L, row, key, data ? (const char*)data : "", (size_t)sqlite3_column_bytes(stmt, c));
                break;
            }
            default: break;
            }
        }
        hks::SetIndex(L, result, ++rowCount, row);
        hks::Pop(L, 1);
    }
    if (rc != SQLITE_DONE) {
        std::string error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        hks::Pop(L, 1);
        return Fail(L, error);
    }
    sqlite3_finalize(stmt);

    hks::SetField(L, result, "changed", hks::Number(sqlite3_changes(db)));
    hks::SetField(L, result, "lastInsertRowId", hks::Number((double)sqlite3_last_insert_rowid(db)));
    LogDebug("database: query on %u returned %d rows: %s", it->first, rowCount, sql.c_str());
    return 1;
}
