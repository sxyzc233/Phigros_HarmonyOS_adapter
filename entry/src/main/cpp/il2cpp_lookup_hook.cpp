/*
 * Hook for Unity's internal symbol lookup.
 *
 * The engine resolves its il2cpp entry points through a lookup helper at libunity+0x670a4c, and every one
 * of those lookups fails here: the helper cannot see libil2cpp, so it stores null into ~127 slots that are
 * later called without a null check. Repairing the slots from outside does not hold, because the engine
 * re-registers them during nativeRender - after any repair we can make.
 *
 * So the helper itself is redirected. A trampoline holds the instructions displaced by the branch, so the
 * original behaviour is preserved; when the original returns null the symbol is looked up directly in
 * libil2cpp's handle, which does succeed.
 */
#include <cstdint>
#include <cstring>

#include <dlfcn.h>
#include <hilog/log.h>
#include <sys/mman.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransLookup"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

const unsigned long kLookupOffset = 0x670a4cUL;   /* Unity's module-table lookup */
const int kDisplacedBytes = 16;                   /* four instructions moved to the trampoline */

void *g_trampoline = nullptr;
void *g_il2cppHandle = nullptr;
int g_fallbackHits = 0;

} // namespace

/* Replacement for the lookup: run the original, then fall back to libil2cpp. */
extern "C" void *ReaIl2cppLookupHook(void *module, const char *name, int flag)
{
    void *result = nullptr;
    if (g_trampoline) {
        using LookupFn = void *(*)(void *, const char *, int);
        result = ((LookupFn)g_trampoline)(module, name, flag);
    }
    if (!result && name && g_il2cppHandle) {
        result = dlsym(g_il2cppHandle, name);
        if (result) {
            g_fallbackHits++;
            if (g_fallbackHits <= 5) {
                LOGI("lookup fallback resolved %s = %p", name, result);
            }
        }
    }
    return result;
}

/*
 * Installs the hook. Returns false when the target page cannot be made writable or the branch is out of
 * range, in which case the caller simply continues without it.
 */
extern "C" bool ReaHookIl2cppLookup(unsigned long unityBase, void *il2cppHandle)
{
    if (!unityBase || !il2cppHandle) return false;
    if (g_trampoline) return true;
    g_il2cppHandle = il2cppHandle;

    unsigned char *target = (unsigned char *)(unityBase + kLookupOffset);
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;

    void *page = mmap(nullptr, (size_t)pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        LOGW("trampoline mmap failed");
        return false;
    }

    /* Copy the displaced instructions, then jump back to the instruction after them. */
    memcpy(page, target, kDisplacedBytes);
    unsigned long *tail = (unsigned long *)((unsigned char *)page + kDisplacedBytes);
    tail[0] = 0x58000050UL;                              /* ldr x16, #8 */
    tail[1] = 0xD61F0200UL;                              /* br x16 */
    tail[2] = (unsigned long)(uintptr_t)(target + kDisplacedBytes);

    void *codePage = (void *)((unsigned long)(uintptr_t)target & ~((unsigned long)pageSize - 1));
    bool restoredExec = true;
    if (mprotect(codePage, (size_t)pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        /* W^X forbids writable+executable. Confirm execute can be restored before touching the code:
         * losing it would corrupt the library, which is worse than not installing the hook. */
        if (mprotect(codePage, (size_t)pageSize, PROT_READ | PROT_WRITE) != 0) {
            LOGW("lookup hook: page cannot be made writable, skipping");
            munmap(page, (size_t)pageSize);
            return false;
        }
        if (mprotect(codePage, (size_t)pageSize, PROT_READ | PROT_EXEC) != 0) {
            LOGW("lookup hook: execute permission cannot be restored, skipping");
            munmap(page, (size_t)pageSize);
            return false;
        }
        /* The round trip works, so repeat it around the actual write. */
        if (mprotect(codePage, (size_t)pageSize, PROT_READ | PROT_WRITE) != 0) {
            LOGW("lookup hook: second mprotect failed, skipping");
            munmap(page, (size_t)pageSize);
            return false;
        }
        restoredExec = false;
    }

    long delta = (long)((unsigned char *)&ReaIl2cppLookupHook - target);
    if (delta < -134217728L || delta >= 134217728L) {
        LOGW("lookup hook branch out of range");
        munmap(page, (size_t)pageSize);
        return false;
    }
    uint32_t branch = 0x14000000U | ((uint32_t)(delta >> 2) & 0x03FFFFFFU);
    memcpy(target, &branch, 4);
    __builtin___clear_cache((char *)target, (char *)target + 4);

    if (!restoredExec) {
        if (mprotect(codePage, (size_t)pageSize, PROT_READ | PROT_EXEC) != 0) {
            LOGW("cannot restore execute permission on the lookup page");
            munmap(page, (size_t)pageSize);
            return false;
        }
    }

    g_trampoline = page;
    LOGI("lookup hook installed at 0x%lx (trampoline %p)", unityBase + kLookupOffset, page);
    return true;
}