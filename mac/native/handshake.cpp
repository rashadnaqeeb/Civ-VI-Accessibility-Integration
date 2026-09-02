// Hook-free lua_State capture. CAI's Lua (caiUtils.lua) calls
// pcall(os.date, "ExposedMembers", 1234567890). Havok Script's os_date calls
// libc gmtime/localtime (a cross-image call the dylib interposes) while
// lua_State* L is still live in a callee-saved register of os_date's frame.
// The naked stubs below save x0..x30; the C side validates the candidate and
// injects ExposedMembers.CAI. See mac/README.md.
//
// The game imports all four of gmtime, gmtime_r, localtime and localtime_r
// (nm -u Civ6_Exe_Child), and which one os_date reaches depends on the
// format string and the libc version, so all four are interposed.
#include "cai.h"
#include "hks.h"
#include <cstdint>
#include <ctime>
#include <mach/mach.h>
#include <mach/mach_vm.h>

// Must fit in 32 bits: os.date truncates its time argument.
constexpr int64_t kMagicTime = 1234567890LL;
// __PAGEZERO covers the first 4 GB of a 64-bit process, so no pointer the
// game holds is below it; the check rejects small integers and flags.
constexpr uint64_t kPageZeroEnd = 0x100000000ULL;
// Bytes of the candidate that CaiLooksLikeState reads: the layout up to and
// including the nextState link.
constexpr size_t kStateProbeBytes = hks::OFF_NEXTSTATE + sizeof(void*);
// The registers the stub spills, in order: x0 to x30.
struct Regs { uint64_t x[31]; };

static bool Readable(const void* p, size_t len) {
    mach_vm_address_t addr = (mach_vm_address_t)p;
    mach_vm_size_t size = len;
    mach_vm_address_t region = addr;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objName;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &objName);
    if (kr != KERN_SUCCESS) return false;
    if (region > addr) return false;
    if (!(info.protection & VM_PROT_READ)) return false;
    return addr + len <= region + size;
}

// Called from the naked stubs with the saved register block, on every thread
// that calls one of the four time functions. The time pointer is the caller's
// own argument, which libc dereferences right after us, so it needs no probe;
// only the register candidates are probed before they are read. Nothing on
// this path may throw: the naked stubs carry no unwind information.
extern "C" void cai_time_capture(const Regs* r, const time_t* t) {
    if (!CaiIsActive()) return;
    if (!t || *t != kMagicTime) return;
    for (int i = 19; i <= 28; ++i) {
        const void* p = (const void*)r->x[i];
        if (r->x[i] > kPageZeroEnd && Readable(p, kStateProbeBytes) && CaiLooksLikeState(p)) {
            CaiInject((lua_State*)p);
            return;
        }
    }
    Log("handshake: no lua_State candidate among x19..x28");
}

// A naked stub for one time function: spill x0..x30, call the capture with the
// block and the time argument, restore x0, x1, fp and lr, and tail-call the
// real function. The _r variants pass their second argument through x1.
#define CAI_TIME_STUB(stub, target)                                   \
    __attribute__((naked)) extern "C" void stub(void) {               \
        __asm__ volatile(                                             \
            "sub sp, sp, #0x100\n"                                    \
            "stp x0, x1, [sp, #0x00]\n"                               \
            "stp x2, x3, [sp, #0x10]\n"                               \
            "stp x4, x5, [sp, #0x20]\n"                               \
            "stp x6, x7, [sp, #0x30]\n"                               \
            "stp x8, x9, [sp, #0x40]\n"                               \
            "stp x10, x11, [sp, #0x50]\n"                             \
            "stp x12, x13, [sp, #0x60]\n"                             \
            "stp x14, x15, [sp, #0x70]\n"                             \
            "stp x16, x17, [sp, #0x80]\n"                             \
            "stp x18, x19, [sp, #0x90]\n"                             \
            "stp x20, x21, [sp, #0xa0]\n"                             \
            "stp x22, x23, [sp, #0xb0]\n"                             \
            "stp x24, x25, [sp, #0xc0]\n"                             \
            "stp x26, x27, [sp, #0xd0]\n"                             \
            "stp x28, x29, [sp, #0xe0]\n"                             \
            "str x30, [sp, #0xf0]\n"                                  \
            "mov x1, x0\n"                                            \
            "mov x0, sp\n"                                            \
            "bl _cai_time_capture\n"                                  \
            "ldp x0, x1, [sp, #0x00]\n"                               \
            "ldp x29, x30, [sp, #0xe8]\n"                             \
            "add sp, sp, #0x100\n"                                    \
            "b _" #target "\n");                                      \
    }                                                                 \
    CAI_INTERPOSE(stub, target)

extern "C" struct tm* gmtime(const time_t*);
extern "C" struct tm* localtime(const time_t*);
extern "C" struct tm* gmtime_r(const time_t*, struct tm*);
extern "C" struct tm* localtime_r(const time_t*, struct tm*);

CAI_TIME_STUB(cai_gmtime_stub, gmtime);
CAI_TIME_STUB(cai_localtime_stub, localtime);
CAI_TIME_STUB(cai_gmtime_r_stub, gmtime_r);
CAI_TIME_STUB(cai_localtime_r_stub, localtime_r);
