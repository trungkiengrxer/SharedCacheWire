// Host-side test for wiring the compression chunks of pages that other kexts patch.
//
// Run from the repository root:
//   clang++ -std=c++17 -I Tests/stubs -DPRODUCT_NAME=SharedCacheWire \
//     -DMODULE_VERSION=0.0.0 Tests/wire_test.cpp -o /tmp/wire_test && /tmp/wire_test

#include "../SharedCacheWire/kern_start.cpp"

#include <vector>

static VnodeStub *const mainCache = reinterpret_cast<VnodeStub *>(0x1000);
static VnodeStub *const subCache = reinterpret_cast<VnodeStub *>(0x2000);
static int failures = 0;
static int failIopl = 0;
static bool innerPatches = false;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

// Stands in for the cs_validate_page chain below this hook (WEG, RestrictEvents, ...).
static void innerValidator(vnode_t, memory_object_t, memory_object_offset_t,
                           const void *data, int *, int *, int *) {
    if (innerPatches)
        memcpy(const_cast<uint8_t *>(static_cast<const uint8_t *>(data)) + 0x946, "hwgva", 5);
}

static void *getObject(vnode_t vnode, int) { return vnode; }

static uint64_t lastIoplFlags = 0;
static int lastIoplTag = 0;
static upl_t issuedUpls[64];
static int issuedUplCount = 0;

static kern_return_t iopl(void *object, uint64_t offset, uint32_t size, upl_t *upl, void *,
                          unsigned int *, upl_control_flags_t flags, uint16_t tag) {
    lastIoplFlags = flags;
    lastIoplTag = tag;
    if (failIopl > 0) {
        failIopl--;
        *upl = nullptr;
        return KERN_MEMORY_ERROR;
    }
    recordEvent('W', static_cast<vnode_t>(object), offset, size);
    *upl = reinterpret_cast<upl_t>(0x5000 + issuedUplCount);
    issuedUpls[issuedUplCount++] = *upl;
    return KERN_SUCCESS;
}

static int shutdownNotifications = 0;
static void originalShutdown(int, int) { shutdownNotifications++; }

static int countCommits(upl_t upl) {
    int n = 0;
    for (int i = 0; i < committedUplCount; i++)
        n += committedUpls[i] == upl;
    return n;
}

static int countReleases(vnode_t vnode) {
    int n = 0;
    for (int i = 0; i < vnodeReles; i++)
        n += releasedVnodes[i] == vnode;
    return n;
}

static void loadPage(vnode_t vnode, uint64_t pageOffset, bool patched) {
    std::vector<uint8_t> page(PAGE_SIZE, 0xCC);
    memcpy(page.data() + 0x946, "board", 5);
    innerPatches = patched;
    patchedCsValidatePage(vnode, nullptr, pageOffset, page.data(), nullptr, nullptr, nullptr);
}

static int countEvents(char kind) {
    int n = 0;
    for (int i = 0; i < kernelEventCount; i++)
        n += kernelEvents[i].kind == kind;
    return n;
}

static const KernelEvent &lastEvent() { return kernelEvents[kernelEventCount - 1]; }

int main() {
    originalCsValidatePage = reinterpret_cast<mach_vm_address_t>(innerValidator);
    ubcGetObject = getObject;
    objectIoplRequest = iopl;
    wireLock = IOLockAlloc();
    chunkLock = IOSimpleLockAlloc();
    wireCall = thread_call_allocate(wireChunks, nullptr);

    loadPage(mainCache, 0x158d6000, false);
    check(threadCallEnters == 0 && chunkCount == 0, "an unpatched page wires nothing");

    loadPage(mainCache, 0x158d6000, true);
    check(threadCallEnters == 1, "a patched page schedules wiring");

    wireChunks(nullptr, nullptr);
    check(kernelEventCount == 2 &&
          kernelEvents[0].kind == 'R' && kernelEvents[0].offset == 0x158d0000 && kernelEvents[0].size == 0x10000 &&
          kernelEvents[1].kind == 'W' && kernelEvents[1].offset == 0x158d0000 && kernelEvents[1].size == 0x10000,
          "the whole chunk is read through the file, then wired");
    check(lastIoplFlags == (UPL_SET_IO_WIRE | UPL_SET_LITE | UPL_SET_INTERNAL | UPL_COPYOUT_FROM) &&
          lastIoplTag == VM_KERN_MEMORY_FILE,
          "the chunk is wired read-only");
    check(vnodeRefs == 1, "a wired chunk keeps its vnode");

    int before = kernelEventCount;
    loadPage(mainCache, 0x158df000, true);
    wireChunks(nullptr, nullptr);
    check(kernelEventCount == before && threadCallEnters == 1, "a second patched page in a wired chunk changes nothing");

    loadPage(subCache, 0x158d6000, true);
    wireChunks(nullptr, nullptr);
    check(countEvents('W') == 2 && lastEvent().vnode == subCache && lastEvent().offset == 0x158d0000,
          "the same offset in another file is wired separately");

    failIopl = 1;
    loadPage(subCache, 0x0c650000, true);
    wireChunks(nullptr, nullptr);
    check(threadCallDelayedEnters == 1 && countEvents('W') == 2, "failed wiring is retried later");
    wireChunks(nullptr, nullptr);
    check(countEvents('W') == 3 && lastEvent().offset == 0x0c650000, "the retry wires the chunk");

    VnodeStub *const gone = reinterpret_cast<VnodeStub *>(0x3000);
    loadPage(gone, 0x20000, true);
    recycledVnode = gone;
    int delayedBefore = threadCallDelayedEnters;
    before = kernelEventCount;
    wireChunks(nullptr, nullptr);
    check(kernelEventCount == before && threadCallDelayedEnters == delayedBefore,
          "a recycled vnode is skipped without retrying");

    failIopl = 1000;
    loadPage(subCache, 0x30000, true);
    for (int i = 0; i < 40; i++)
        wireChunks(nullptr, nullptr);
    check(threadCallDelayedEnters - delayedBefore == maxWireAttempts - 1,
          "wiring gives up after the attempt limit");

    // Kept so far: mainCache 0x158d0000, subCache 0x158d0000 and 0x0c650000.
    originalShutdownNotification = reinterpret_cast<mach_vm_address_t>(originalShutdown);
    failIopl = 0;
    patchedShutdownNotification(0, 0);
    check(committedUplCount == 0 && vnodeReles == 0 && shutdownNotifications == 1,
          "shutdown stage 0 releases nothing");

    patchedShutdownNotification(0, 1);
    bool eachOnce = issuedUplCount == 3 && committedUplCount == 3;
    for (int i = 0; i < issuedUplCount; i++)
        eachOnce = eachOnce && countCommits(issuedUpls[i]) == 1;
    check(eachOnce && shutdownNotifications == 2, "shutdown stage 1 commits every kept UPL once");
    check(vnodeReles == vnodeRefs && countReleases(mainCache) == 1 && countReleases(subCache) == 2 &&
          countReleases(gone) == 0,
          "shutdown stage 1 releases every kept vnode once");
    check(threadCallCancels == 1, "shutdown stage 1 cancels pending wiring");

    patchedShutdownNotification(0, 1);
    patchedShutdownNotification(0, 2);
    check(committedUplCount == 3 && vnodeReles == 3 && threadCallCancels == 1 && shutdownNotifications == 4,
          "later shutdown notifications release nothing again");

    before = kernelEventCount;
    loadPage(mainCache, 0x40000, true);
    wireChunks(nullptr, nullptr);
    check(kernelEventCount == before && vnodeRefs == 3, "nothing is wired after shutdown stage 1");

    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
