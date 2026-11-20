// Host-side stand-ins for the Lilu and XNU APIs used by kern_start.cpp.
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define xStringify(a) #a
#define ADDPR(a) a
#define SYSLOG(...) do {} while (0)
#define DBGLOG(...) do {} while (0)

typedef uint64_t mach_vm_address_t;
typedef struct VnodeStub *vnode_t;
typedef void *memory_object_t;
typedef uint64_t memory_object_offset_t;

template <typename T, size_t N>
constexpr size_t arrsize(const T (&)[N]) { return N; }

template <typename T>
T FunctionCast(T, mach_vm_address_t original) {
    return reinterpret_cast<T>(original);
}

enum class KernelVersion { BigSur = 20, Tahoe = 25 };
inline bool checkKernelArgument(const char *) { return false; }

struct KernelPatcher {
    static constexpr size_t KernelID = 0;

    struct RouteRequest {
        template <typename T>
        RouteRequest(const char *, T, mach_vm_address_t &) {}
    };

    bool routeMultipleLong(size_t, RouteRequest *, size_t) { return true; }
    mach_vm_address_t solveSymbol(size_t, const char *) { return 0; }
    void clearError() {}
};

typedef int kern_return_t;
#define KERN_SUCCESS 0
#define KERN_MEMORY_ERROR 10

typedef uint64_t upl_control_flags_t;
typedef struct UplStub *upl_t;
#define UPL_COPYOUT_FROM 0x00000001ULL
#define UPL_SET_INTERNAL 0x00000040ULL
#define UPL_SET_LITE     0x00000400ULL
#define UPL_SET_IO_WIRE  0x00001000ULL
#define VM_KERN_MEMORY_FILE 20

typedef struct ThreadCallStub *thread_call_t;
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
inline int threadCallEnters = 0;
inline int threadCallDelayedEnters = 0;
inline thread_call_t thread_call_allocate(thread_call_func_t, thread_call_param_t) {
    return reinterpret_cast<thread_call_t>(0x3000);
}
inline bool thread_call_enter(thread_call_t) { threadCallEnters++; return false; }
inline bool thread_call_enter_delayed(thread_call_t, uint64_t) { threadCallDelayedEnters++; return false; }
inline int threadCallCancels = 0;
inline bool thread_call_cancel(thread_call_t) { threadCallCancels++; return false; }
inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *deadline) { *deadline = 1; }
enum { kSecondScale = 1000 * 1000 * 1000 };

struct IOLock {};
inline IOLock *IOLockAlloc() { static IOLock lock; return &lock; }
inline void IOLockLock(IOLock *) {}
inline void IOLockUnlock(IOLock *) {}

struct IOSimpleLock {};
inline IOSimpleLock *IOSimpleLockAlloc() { static IOSimpleLock lock; return &lock; }
inline void IOSimpleLockLock(IOSimpleLock *) {}
inline void IOSimpleLockUnlock(IOSimpleLock *) {}

// A vnode's id is its address; a recycled vnode no longer matches it.
inline vnode_t recycledVnode = nullptr;
typedef void *kauth_cred_t;
typedef void *proc_t;
enum uio_rw { UIO_READ, UIO_WRITE };
enum uio_seg { UIO_SYSSPACE = 2 };
inline kauth_cred_t kauth_cred_get() { return nullptr; }
inline uint32_t vnode_vid(vnode_t vnode) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vnode)); }
inline int vnode_getwithvid(vnode_t vnode, uint32_t vid) {
    return vnode != recycledVnode && vid == vnode_vid(vnode) ? 0 : 2;
}
inline int vnode_put(vnode_t) { return 0; }
inline int vnodeRefs = 0;
inline int vnode_ref(vnode_t) { vnodeRefs++; return 0; }
inline vnode_t releasedVnodes[64];
inline int vnodeReles = 0;
inline void vnode_rele(vnode_t vnode) {
    if (vnodeReles < 64)
        releasedVnodes[vnodeReles++] = vnode;
}
inline upl_t committedUpls[64];
inline int committedUplCount = 0;
inline int ubc_upl_commit(upl_t upl) {
    if (committedUplCount < 64)
        committedUpls[committedUplCount++] = upl;
    return 0;
}
inline int vn_getpath(vnode_t, char *path, int *) { strcpy(path, "file"); return 0; }

// Ordered log of reads and wires: {'R' or 'W', vnode, file offset, size}.
struct KernelEvent { char kind; vnode_t vnode; uint64_t offset; uint64_t size; };
inline KernelEvent kernelEvents[64];
inline int kernelEventCount = 0;
inline void recordEvent(char kind, vnode_t vnode, uint64_t offset, uint64_t size) {
    if (kernelEventCount < 64)
        kernelEvents[kernelEventCount++] = {kind, vnode, offset, size};
}
inline int vn_rdwr(enum uio_rw, vnode_t vnode, caddr_t, int len, int64_t offset, enum uio_seg, int,
                   kauth_cred_t, int *resid, proc_t) {
    recordEvent('R', vnode, static_cast<uint64_t>(offset), static_cast<uint64_t>(len));
    *resid = 0;
    return 0;
}

struct LiluAPI {
    enum Requirements { AllowNormal };
    void onPatcherLoadForce(void (*)(void *, KernelPatcher &)) {}
};
inline LiluAPI lilu;

inline size_t parseModuleVersion(const char *) { return 0; }

struct PluginConfiguration {
    const char *product;
    size_t version;
    LiluAPI::Requirements requirements;
    const char **disableArgs;
    size_t disableArgCount;
    const char **debugArgs;
    size_t debugArgCount;
    const char **betaArgs;
    size_t betaArgCount;
    KernelVersion minKernel;
    KernelVersion maxKernel;
    void (*start)();
};
