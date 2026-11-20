//
//  kern_start.cpp
//  SharedCacheWire.kext
//
// Keeps patched file-backed code pages from killing their neighbours.
//
// Kexts such as WhateverGreen, RestrictEvents and RealtekUVCFix patch code in
// the page cache from a _cs_validate_page hook. The dyld shared cache is an
// APFS-compressed file, so once a patched page is resident, macOS cannot page
// in an evicted neighbour from the same compression chunk: APFS reports
// "compressed data hash mismatch" and the faulting process dies with SIGBUS
// ("FS pagein error: 94 Bad message").
//
// This kext hooks _cs_validate_page above every other patcher, notices when
// the page changed while the hooks below it ran, and wires that page's whole
// compression chunk until shutdown, so none of its pages is paged in again.
// The wires are released at shutdown stage 1, before volumes unmount.
//
// Load order matters: this kext must come after every other kext that hooks
// _cs_validate_page (last in OpenCore Kernel > Add). A patcher loaded later
// sits above this hook, and its patches go unnoticed.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/memory_object_types.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#ifndef VM_KERN_MEMORY_FILE
#define VM_KERN_MEMORY_FILE 20
#endif

#define MODULE_SHORT "scw"

static mach_vm_address_t originalCsValidatePage {};
static mach_vm_address_t originalShutdownNotification {};

// ponytail: assumes 64 KB APFS compression chunks. Raise compressionChunk if
// a future macOS uses larger ones.
static constexpr uint64_t compressionChunk = 0x10000;
static constexpr uint32_t maxWireAttempts = 30;

// ponytail: fixed table, 64 chunks (4 MB wired at most). Raise maxChunks if
// the "chunk table is full" log line ever shows up.
static constexpr size_t maxChunks = 64;

enum class ChunkState : uint8_t { Pending, Wired, Dropped };

struct Chunk {
    vnode_t vnode;
    uint32_t vid;
    uint64_t offset;
    ChunkState state;
    uint32_t attempts;
    upl_t upl;  // held while Wired
};

using t_ubcGetObject = void *(*)(vnode_t, int);
using t_objectIoplRequest = kern_return_t (*)(
    void *object, uint64_t offset, uint32_t size, upl_t *upl, void *pageList,
    unsigned int *pageListCount, upl_control_flags_t flags, uint16_t tag);

static t_ubcGetObject ubcGetObject {};
static t_objectIoplRequest objectIoplRequest {};
static thread_call_t wireCall {};
static IOLock *wireLock {};
static bool shuttingDown {};  // under wireLock; set once, nothing wires after

// The hook appends under chunkLock. Entries never move, vnode/vid/offset never
// change after the append, and state, attempts and upl change only under
// wireLock.
static IOSimpleLock *chunkLock {};
static Chunk chunks[maxChunks] {};
static size_t chunkCount {};
static bool chunkTableFull {};

// FNV-1a over the page's 64-bit words. Each step is a bijection of the state,
// so a patch confined to one word always changes the hash.
static uint64_t pageHash(const void *data) {
    auto words = static_cast<const uint64_t *>(data);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
        hash = (hash ^ words[i]) * 0x100000001b3ULL;
    return hash;
}

// Records the chunk unless it is already known. Returns true for a new chunk.
static bool noteChunk(vnode_t vnode, uint64_t offset) {
    uint32_t vid = vnode_vid(vnode);
    bool added = false;

    IOSimpleLockLock(chunkLock);
    bool known = false;
    for (size_t i = 0; i < chunkCount && !known; i++)
        known = chunks[i].vnode == vnode && chunks[i].vid == vid && chunks[i].offset == offset;

    if (!known && chunkCount < maxChunks) {
        chunks[chunkCount] = {vnode, vid, offset, ChunkState::Pending, 0, nullptr};
        chunkCount++;
        added = true;
    } else if (!known) {
        chunkTableFull = true;
    }
    IOSimpleLockUnlock(chunkLock);

    return added;
}

// Returns the UPL that keeps the chunk wired, or nullptr on failure.
static upl_t wireChunk(vnode_t vnode, uint64_t offset) {
    // Serialised by wireLock.
    static uint8_t buffer[compressionChunk];
    int resid = 0;

    // Reading through the file still works where a page fault fails, and it
    // brings the whole chunk into the page cache before it is wired.
    int error = vn_rdwr(UIO_READ, vnode, reinterpret_cast<caddr_t>(buffer),
                        static_cast<int>(compressionChunk), static_cast<off_t>(offset),
                        UIO_SYSSPACE, 0, kauth_cred_get(), &resid,
                        nullptr);  // only consulted for user-space buffers
    if (error != 0) {
        SYSLOG(MODULE_SHORT, "reading chunk at 0x%llx failed with %d",
               static_cast<unsigned long long>(offset), error);
        return nullptr;
    }

    void *object = ubcGetObject(vnode, 0);
    if (object == nullptr)
        return nullptr;

    // Read-only (UPL_COPYOUT_FROM): neither pushes copy-on-write copies nor
    // taints pages. Never wire these pages for write or with mlock(): that
    // leaves unsigned page copies that get processes killed until reboot.
    // The UPL is held until shutdown on purpose: releasing it unwires the
    // chunk.
    upl_t upl {};
    kern_return_t result = objectIoplRequest(
        object, offset, static_cast<uint32_t>(compressionChunk), &upl, nullptr, nullptr,
        UPL_SET_IO_WIRE | UPL_SET_LITE | UPL_SET_INTERNAL | UPL_COPYOUT_FROM,
        VM_KERN_MEMORY_FILE);

    if (result != KERN_SUCCESS) {
        SYSLOG(MODULE_SHORT, "wiring chunk at 0x%llx failed with %d",
               static_cast<unsigned long long>(offset), result);
        return nullptr;
    }

    char path[PATH_MAX] {};
    int pathLength = sizeof(path);
    if (vn_getpath(vnode, path, &pathLength) != 0)
        path[0] = '\0';
    SYSLOG(MODULE_SHORT, "wired chunk at 0x%llx of %s",
           static_cast<unsigned long long>(offset), path);
    return upl;
}

static void wireChunks(thread_call_param_t, thread_call_param_t) {
    IOLockLock(wireLock);
    if (shuttingDown) {
        IOLockUnlock(wireLock);
        return;
    }

    IOSimpleLockLock(chunkLock);
    size_t count = chunkCount;
    bool full = chunkTableFull;
    chunkTableFull = false;
    IOSimpleLockUnlock(chunkLock);

    if (full)
        SYSLOG(MODULE_SHORT, "chunk table is full, newer patched chunks stay unwired");

    bool retry = false;
    for (size_t i = 0; i < count; i++) {
        Chunk &chunk = chunks[i];
        if (chunk.state != ChunkState::Pending)
            continue;

        // A recycled vnode took its patched pages with it.
        if (vnode_getwithvid(chunk.vnode, chunk.vid) != 0) {
            chunk.state = ChunkState::Dropped;
            continue;
        }

        chunk.upl = wireChunk(chunk.vnode, chunk.offset);
        if (chunk.upl != nullptr) {
            // Keep the vnode, and with it the VM object the UPL points to.
            vnode_ref(chunk.vnode);
            chunk.state = ChunkState::Wired;
        } else if (++chunk.attempts >= maxWireAttempts) {
            SYSLOG(MODULE_SHORT, "giving up on chunk at 0x%llx after %u attempts",
                   static_cast<unsigned long long>(chunk.offset), chunk.attempts);
            chunk.state = ChunkState::Dropped;
        } else {
            retry = true;
        }

        vnode_put(chunk.vnode);
    }

    if (retry) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(1, kSecondScale, &deadline);
        thread_call_enter_delayed(wireCall, deadline);
    }

    IOLockUnlock(wireLock);
}

// reboot_kernel sends stage 1 after killing every process and syncing, right
// before vfs_unmountall. The wired chunks pin Preboot (where the dyld shared
// cache lives), so they must go first or its unmount waits forever.
static void patchedShutdownNotification(int howto, int stage) {
    if (stage == 1 && wireLock != nullptr) {
        IOLockLock(wireLock);
        if (!shuttingDown) {
            shuttingDown = true;
            thread_call_cancel(wireCall);

            IOSimpleLockLock(chunkLock);
            size_t count = chunkCount;
            IOSimpleLockUnlock(chunkLock);

            for (size_t i = 0; i < count; i++) {
                Chunk &chunk = chunks[i];
                if (chunk.state != ChunkState::Wired)
                    continue;
                // Committing an IO-wired UPL unwires its pages and frees it.
                ubc_upl_commit(chunk.upl);
                vnode_rele(chunk.vnode);
                chunk.upl = nullptr;
                chunk.state = ChunkState::Dropped;
            }
        }
        IOLockUnlock(wireLock);
    }

    FunctionCast(patchedShutdownNotification, originalShutdownNotification)(howto, stage);
}

static void patchedCsValidatePage(vnode_t vnode, memory_object_t pager, memory_object_offset_t pageOffset,
                                  const void *data, int *validated, int *tainted, int *nx) {
    uint64_t before = data != nullptr ? pageHash(data) : 0;

    FunctionCast(patchedCsValidatePage, originalCsValidatePage)(
        vnode, pager, pageOffset, data, validated, tainted, nx);

    if (data == nullptr || vnode == nullptr || pageHash(data) == before)
        return;

    // Wiring blocks and takes VM locks, so it cannot run inside the fault.
    if (noteChunk(vnode, pageOffset & ~(compressionChunk - 1)))
        thread_call_enter(wireCall);
}

static void pluginStart() {
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        ubcGetObject = reinterpret_cast<t_ubcGetObject>(
            patcher.solveSymbol(KernelPatcher::KernelID, "_ubc_getobject"));
        objectIoplRequest = reinterpret_cast<t_objectIoplRequest>(
            patcher.solveSymbol(KernelPatcher::KernelID, "_vm_object_iopl_request"));
        patcher.clearError();

        wireLock = IOLockAlloc();
        chunkLock = IOSimpleLockAlloc();
        if (ubcGetObject == nullptr || objectIoplRequest == nullptr ||
            wireLock == nullptr || chunkLock == nullptr) {
            SYSLOG(MODULE_SHORT, "wiring is unavailable, not hooking");
            return;
        }

        wireCall = thread_call_allocate(wireChunks, nullptr);
        if (wireCall == nullptr) {
            SYSLOG(MODULE_SHORT, "failed to allocate the wiring thread call");
            return;
        }

        // Without the shutdown hook nothing would release the wires and
        // shutdown would hang, so never wire in that case.
        KernelPatcher::RouteRequest shutdown("_IOSystemShutdownNotification", patchedShutdownNotification,
                                             originalShutdownNotification);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &shutdown, 1)) {
            SYSLOG(MODULE_SHORT, "failed to route _IOSystemShutdownNotification, not hooking");
            return;
        }

        KernelPatcher::RouteRequest request("_cs_validate_page", patchedCsValidatePage, originalCsValidatePage);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1))
            SYSLOG(MODULE_SHORT, "failed to route _cs_validate_page");
    });
}

static const char *bootargOff[] {
    "-scwoff"
};

static const char *bootargDebug[] {
    "-scwdbg"
};

static const char *bootargBeta[] {
    "-scwbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::BigSur,
    KernelVersion::Tahoe,
    pluginStart
};
