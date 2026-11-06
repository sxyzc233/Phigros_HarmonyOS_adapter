#include <native_window/external_window.h>
/*
 * pthread ABI translation for Android libraries hosted on HarmonyOS.
 *
 * The incompatibility: bionic's pthread_mutex_t is 4 bytes, while HarmonyOS's musl declares
 * 40 bytes on 64-bit (int __i[10]). Unity is built against bionic, so every mutex it allocates is a
 * 4-byte object, and HarmonyOS's implementation happily reads and writes offsets 4 and 8 - straight
 * into whatever globals sit next to it. The same applies to condition variables and rwlocks.
 *
 * Interposing these symbols by exporting them does not work: the process already has libc in its
 * global scope, so libunity's PLT binds to libc first. Instead, the GOT slots of libunity are
 * rewritten after load (see ReaPatchPthreadGOT), which redirects exactly the library we care about
 * and leaves the rest of the process untouched.
 *
 * Each foreign object address gets a real HarmonyOS object allocated on the side, created lazily so
 * zero-initialised statics work without an init call. Mutexes are created recursive: that is a
 * superset of normal behaviour and avoids deadlocks if a recursive mutex is requested.
 */
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstddef>

#include <dlfcn.h>
#include <hilog/log.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <cerrno>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <new>
#include <sys/socket.h>
struct AndroidAddrInfo;
extern "C" int rea_getaddrinfo(const char *,const char *,const AndroidAddrInfo *,AndroidAddrInfo **);
extern "C" void rea_freeaddrinfo(AndroidAddrInfo *);
extern "C" const char *rea_gai_strerror(int);
extern "C" int rea_getnameinfo(const sockaddr *,socklen_t,char *,size_t,char *,size_t,int);
extern "C" int rea_net_connect(int,const sockaddr *,socklen_t);
extern "C" int ReaNetworkSelfTest();
extern "C" long rea_sysconf(int);
extern "C" int ReaSysconfSelfTest();
extern "C" void *ReaExceptionThrowHook();

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransPthread"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

/* ---- real implementations, resolved from libc by handle so our own exports cannot recurse ---- */

static int g_condTrace = 0;

struct RealApi {
    int (*mutex_init)(pthread_mutex_t *, const void *);
    int (*mutex_lock)(pthread_mutex_t *);
    int (*mutex_trylock)(pthread_mutex_t *);
    int (*mutex_unlock)(pthread_mutex_t *);
    int (*mutex_destroy)(pthread_mutex_t *);
    int (*thread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    int (*cond_init)(pthread_cond_t *, const void *);
    int (*cond_wait)(pthread_cond_t *, pthread_mutex_t *);
    int (*cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
    int (*cond_signal)(pthread_cond_t *);
    int (*cond_broadcast)(pthread_cond_t *);
    int (*cond_destroy)(pthread_cond_t *);
    int (*rwlock_init)(pthread_rwlock_t *, const void *);
    int (*rwlock_rdlock)(pthread_rwlock_t *);
    int (*rwlock_wrlock)(pthread_rwlock_t *);
    int (*rwlock_unlock)(pthread_rwlock_t *);
    int (*rwlock_destroy)(pthread_rwlock_t *);
    int (*sigaction)(int, const struct sigaction *, struct sigaction *);
    int (*sem_init)(sem_t *, int, unsigned);
    int (*sem_wait)(sem_t *);
    int (*sem_trywait)(sem_t *);
    int (*sem_timedwait)(sem_t *, const struct timespec *);
    int (*sem_post)(sem_t *);
    int (*sem_destroy)(sem_t *);
    int (*sem_getvalue)(sem_t *, int *);
    bool ready;
};

RealApi g_real;

void ResolveReal()
{
    if (g_real.ready) return;
    void *libc = dlopen("libc.so", RTLD_NOLOAD | RTLD_LAZY);
    if (!libc) libc = dlopen("libc.so", RTLD_LAZY);
    if (!libc) {
        LOGW("cannot open libc for the real pthread entry points");
        return;
    }
    g_real.mutex_init = (int (*)(pthread_mutex_t *, const void *))dlsym(libc, "pthread_mutex_init");
    g_real.mutex_lock = (int (*)(pthread_mutex_t *))dlsym(libc, "pthread_mutex_lock");
    g_real.mutex_trylock = (int (*)(pthread_mutex_t *))dlsym(libc, "pthread_mutex_trylock");
    g_real.mutex_unlock = (int (*)(pthread_mutex_t *))dlsym(libc, "pthread_mutex_unlock");
    g_real.mutex_destroy = (int (*)(pthread_mutex_t *))dlsym(libc, "pthread_mutex_destroy");
    g_real.thread_create =
        (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))dlsym(libc,
                                                                                     "pthread_create");
    g_real.cond_init = (int (*)(pthread_cond_t *, const void *))dlsym(libc, "pthread_cond_init");
    g_real.cond_wait = (int (*)(pthread_cond_t *, pthread_mutex_t *))dlsym(libc, "pthread_cond_wait");
    g_real.cond_timedwait =
        (int (*)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *))dlsym(libc, "pthread_cond_timedwait");
    g_real.cond_signal = (int (*)(pthread_cond_t *))dlsym(libc, "pthread_cond_signal");
    g_real.cond_broadcast = (int (*)(pthread_cond_t *))dlsym(libc, "pthread_cond_broadcast");
    g_real.cond_destroy = (int (*)(pthread_cond_t *))dlsym(libc, "pthread_cond_destroy");
    g_real.rwlock_init = (int (*)(pthread_rwlock_t *, const void *))dlsym(libc, "pthread_rwlock_init");
    g_real.rwlock_rdlock = (int (*)(pthread_rwlock_t *))dlsym(libc, "pthread_rwlock_rdlock");
    g_real.rwlock_wrlock = (int (*)(pthread_rwlock_t *))dlsym(libc, "pthread_rwlock_wrlock");
    g_real.rwlock_unlock = (int (*)(pthread_rwlock_t *))dlsym(libc, "pthread_rwlock_unlock");
    g_real.rwlock_destroy = (int (*)(pthread_rwlock_t *))dlsym(libc, "pthread_rwlock_destroy");
    g_real.sigaction =
        (int (*)(int, const struct sigaction *, struct sigaction *))dlsym(libc, "sigaction");
    g_real.sem_init = (int (*)(sem_t *, int, unsigned))dlsym(libc, "sem_init");
    g_real.sem_wait = (int (*)(sem_t *))dlsym(libc, "sem_wait");
    g_real.sem_trywait = (int (*)(sem_t *))dlsym(libc, "sem_trywait");
    g_real.sem_timedwait = (int (*)(sem_t *, const struct timespec *))dlsym(libc, "sem_timedwait");
    g_real.sem_post = (int (*)(sem_t *))dlsym(libc, "sem_post");
    g_real.sem_destroy = (int (*)(sem_t *))dlsym(libc, "sem_destroy");
    g_real.sem_getvalue = (int (*)(sem_t *, int *))dlsym(libc, "sem_getvalue");
    g_real.ready = g_real.mutex_lock && g_real.mutex_unlock;
    LOGI("real pthread api resolved: %d", g_real.ready ? 1 : 0);
}

/* ---- side table: foreign object address -> real HarmonyOS object ---------------------------- */

const int kTableSize = 1024;

struct MutexEntry {
    uintptr_t key;
    pthread_mutex_t mutex;
};

struct CondEntry {
    uintptr_t key;
    pthread_cond_t cond;
};

struct RwEntry {
    uintptr_t key;
    pthread_rwlock_t lock;
};

// Node ownership keeps native lock addresses stable as the registry grows.
// A fixed 1024-entry pool exhausted during ordinary song/resource loading.
std::map<uintptr_t,std::unique_ptr<MutexEntry>> g_mutexes;
std::map<uintptr_t,std::unique_ptr<CondEntry>> g_conds;
std::map<uintptr_t,std::unique_ptr<RwEntry>> g_rwlocks;
size_t g_mutexPeak=0,g_condPeak=0,g_rwPeak=0;
constexpr uintptr_t kDeletedSem=~uintptr_t(0);

struct SemEntry {
    uintptr_t key;
    sem_t sem;
    unsigned long posts;
    unsigned long waits;
};

SemEntry g_sems[kTableSize];
int g_spin = 0;
static thread_local sigset_t g_tablePreviousMask;
static thread_local bool g_tableMaskSaved = false;
static int g_tableOwnerTid = 0;

void LockTable()
{
    // The bundled IL2CPP GC suspends foreign threads with SIGPWR (30).
    // A suspend handler can call sem_post through this adapter. Never let it
    // suspend a thread while that thread owns the adapter's mapping-table lock.
    // Restore the exact previous mask after releasing the lock.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPWR);
    g_tableMaskSaved = pthread_sigmask(SIG_BLOCK, &blocked, &g_tablePreviousMask) == 0;
    while (__atomic_test_and_set(&g_spin, __ATOMIC_ACQUIRE)) {
        /* spin */
    }
    __atomic_store_n(&g_tableOwnerTid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
}

void UnlockTable()
{
    __atomic_store_n(&g_tableOwnerTid, 0, __ATOMIC_RELEASE);
    __atomic_clear(&g_spin, __ATOMIC_RELEASE);
    if (g_tableMaskSaved) pthread_sigmask(SIG_SETMASK, &g_tablePreviousMask, nullptr);
}

pthread_mutex_t *MutexFor(void *foreign)
{
    uintptr_t key = (uintptr_t)foreign;
    if (!key) return nullptr;
    auto found=g_mutexes.find(key);if(found!=g_mutexes.end())return &found->second->mutex;
    std::unique_ptr<MutexEntry> entry(new(std::nothrow) MutexEntry{});if(!entry){errno=ENOMEM;return nullptr;}
    entry->key=key;pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    int result=g_real.mutex_init(&entry->mutex,&attr);pthread_mutexattr_destroy(&attr);
    if(result){errno=result;return nullptr;}
    auto *native=&entry->mutex;
    try{g_mutexes.emplace(key,std::move(entry));}catch(const std::bad_alloc &){if(entry)g_real.mutex_destroy(&entry->mutex);errno=ENOMEM;return nullptr;}
    if(g_mutexes.size()>g_mutexPeak)g_mutexPeak=g_mutexes.size();return native;
}

sem_t *SemFor(void *foreign)
{
    uintptr_t key = (uintptr_t)foreign;
    if (!key) return nullptr;
    uintptr_t slot = (key >> 3) % kTableSize;
    SemEntry *available=nullptr;
    for (int i = 0; i < kTableSize; i++) {
        SemEntry &entry = g_sems[(slot + i) % kTableSize];
        if (entry.key == key) return &entry.sem;
        if(entry.key==kDeletedSem&&!available)available=&entry;
        if(entry.key==0){if(!available)available=&entry;break;}
    }
    if(available){int result=g_real.sem_init(&available->sem,0,0);if(result)return nullptr;
        available->posts=available->waits=0;__atomic_store_n(&available->key,key,__ATOMIC_RELEASE);return &available->sem;}
    LOGW("sem table full");
    return nullptr;
}

pthread_rwlock_t *RwlockFor(void *foreign)
{
    uintptr_t key = (uintptr_t)foreign;
    if (!key) return nullptr;
    auto found=g_rwlocks.find(key);if(found!=g_rwlocks.end())return &found->second->lock;
    std::unique_ptr<RwEntry> entry(new(std::nothrow) RwEntry{});if(!entry){errno=ENOMEM;return nullptr;}
    entry->key=key;int result=g_real.rwlock_init(&entry->lock,nullptr);if(result){errno=result;return nullptr;}
    auto *native=&entry->lock;
    try{g_rwlocks.emplace(key,std::move(entry));}catch(const std::bad_alloc &){if(entry)g_real.rwlock_destroy(&entry->lock);errno=ENOMEM;return nullptr;}
    if(g_rwlocks.size()>g_rwPeak)g_rwPeak=g_rwlocks.size();return native;
}

pthread_cond_t *CondFor(void *foreign)
{
    uintptr_t key = (uintptr_t)foreign;
    if (!key) return nullptr;
    auto found=g_conds.find(key);if(found!=g_conds.end())return &found->second->cond;
    std::unique_ptr<CondEntry> entry(new(std::nothrow) CondEntry{});if(!entry){errno=ENOMEM;return nullptr;}
    entry->key=key;int result=g_real.cond_init(&entry->cond,nullptr);if(result){errno=result;return nullptr;}
    auto *native=&entry->cond;
    try{g_conds.emplace(key,std::move(entry));}catch(const std::bad_alloc &){if(entry)g_real.cond_destroy(&entry->cond);errno=ENOMEM;return nullptr;}
    if(g_conds.size()>g_condPeak)g_condPeak=g_conds.size();return native;
}

} // namespace

/* Result of the most recent self-test, kept alive for the caller. */
std::string g_abiSelfTest;

/* ---- exported translations ---------------------------------------------------------------- */

extern "C" int rea_pthread_mutex_init(void *mutex, const void *attr)
{
    (void)attr;
    ResolveReal();
    LockTable();
    pthread_mutex_t *real = MutexFor(mutex);
    UnlockTable();
    return real ? 0 : -1;
}

extern "C" int rea_pthread_mutex_lock(void *mutex)
{
    ResolveReal();
    LockTable();
    pthread_mutex_t *real = MutexFor(mutex);
    UnlockTable();
    return real ? g_real.mutex_lock(real) : -1;
}

extern "C" int rea_pthread_mutex_trylock(void *mutex)
{
    ResolveReal();
    LockTable();
    pthread_mutex_t *real = MutexFor(mutex);
    UnlockTable();
    return real ? g_real.mutex_trylock(real) : -1;
}

extern "C" int rea_pthread_mutex_unlock(void *mutex)
{
    ResolveReal();
    LockTable();
    pthread_mutex_t *real = MutexFor(mutex);
    UnlockTable();
    return real ? g_real.mutex_unlock(real) : -1;
}

extern "C" int rea_pthread_mutex_destroy(void *mutex)
{
    ResolveReal();
    uintptr_t key = (uintptr_t)mutex;
    LockTable();
    int result=0;auto found=g_mutexes.find(key);
    if(found!=g_mutexes.end()){result=g_real.mutex_destroy(&found->second->mutex);if(result==0)g_mutexes.erase(found);}
    UnlockTable();
    return result;
}


extern "C" int rea_pthread_cond_init(void *cond, const void *attr)
{
    (void)attr;
    ResolveReal();
    LockTable();
    pthread_cond_t *real = CondFor(cond);
    UnlockTable();
    return real ? 0 : -1;
}

static volatile pthread_cond_t *g_pendingCond = nullptr;
static volatile bool g_pendingActive = false;
static volatile long long g_pendingSince = 0;
static volatile int g_rescueCount = 0;

static long long NowMs(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Wakes a wait that has been outstanding for too long, as nothing else will. */
static void *CondRescue(void *raw)
{
    (void)raw;
    for (;;) {
        usleep(500000);
        if (!g_pendingActive || !g_pendingCond) continue;
        if (NowMs() - g_pendingSince < 3000) continue;
        if (g_rescueCount >= 20) continue;
        g_rescueCount++;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "TransPthread", "CONDRESCUE #%{public}d", g_rescueCount);
        g_real.cond_signal((pthread_cond_t *)g_pendingCond);
        g_pendingSince = NowMs();
    }
    return nullptr;
}

/* Records thread creation: names change at runtime, start routines do not. */
static int g_createdThreads = 0;
static unsigned long g_suspendSends=0,g_restartSends=0,g_signalSendFailures=0;
extern "C" int rea_pthread_kill(pthread_t thread,int signalNumber)
{
    int result=pthread_kill(thread,signalNumber);
    if (signalNumber==SIGPWR)__atomic_fetch_add(&g_suspendSends,1UL,__ATOMIC_RELAXED);
    if (signalNumber==SIGXCPU)__atomic_fetch_add(&g_restartSends,1UL,__ATOMIC_RELAXED);
    if (result)__atomic_fetch_add(&g_signalSendFailures,1UL,__ATOMIC_RELAXED);
    return result;
}

extern "C" int rea_pthread_create(void *thread, const void *attr, void *(*start)(void *), void *arg)
{
    ResolveReal();
    if (g_real.thread_create) {
        int result = g_real.thread_create((pthread_t *)thread, (const pthread_attr_t *)attr, start, arg);
        g_createdThreads++;
        if (g_createdThreads <= 40) {
            unsigned long address = (unsigned long)(uintptr_t)start;
            Dl_info where;
            memset(&where, 0, sizeof(where));
            const char *module = "?";
            unsigned long offset = 0;
            if (dladdr((void *)(uintptr_t)address, &where) && where.dli_fname) {
                module = where.dli_fname;
                offset = address - (unsigned long)(uintptr_t)where.dli_fbase;
            }
            OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "TransPthread",
                         "THREAD_CREATE #%{public}d start=%{public}s+0x%{public}lx result=%{public}d",
                         g_createdThreads, module, offset, result);
        }
        return result;
    }
    return -1;
}

extern "C" int rea_pthread_cond_wait(void *cond, void *mutex)
{
    ResolveReal();
    LockTable();
    pthread_cond_t *realCond = CondFor(cond);
    pthread_mutex_t *realMutex = MutexFor(mutex);
    UnlockTable();
    if (!realCond || !realMutex) return -1;
    if (g_condTrace < 200) {
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "TransPthread", "CONDWAIT %{public}p on %{public}p", cond,
                     mutex);
        g_condTrace++;
    }
    g_pendingCond = realCond;
    g_pendingSince = NowMs();
    g_pendingActive = true;
    int result = g_real.cond_wait(realCond, realMutex);
    g_pendingActive = false;
    if (g_condTrace < 200) {
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "TransPthread", "CONDWAKE %{public}p result=%{public}d", cond,
                     result);
        g_condTrace++;
    }
    return result;
}

extern "C" int rea_pthread_cond_timedwait(void *cond, void *mutex, const struct timespec *abstime)
{
    ResolveReal();
    LockTable();
    pthread_cond_t *realCond = CondFor(cond);
    pthread_mutex_t *realMutex = MutexFor(mutex);
    UnlockTable();
    if (!realCond || !realMutex) return -1;
    return g_real.cond_timedwait(realCond, realMutex, abstime);
}

extern "C" int rea_pthread_cond_signal(void *cond)
{
    ResolveReal();
    LockTable();
    pthread_cond_t *real = CondFor(cond);
    UnlockTable();
    return real ? g_real.cond_signal(real) : -1;
}

extern "C" int rea_pthread_cond_broadcast(void *cond)
{
    ResolveReal();
    LockTable();
    pthread_cond_t *real = CondFor(cond);
    UnlockTable();
    return real ? g_real.cond_broadcast(real) : -1;
}

extern "C" int rea_pthread_cond_destroy(void *cond)
{
    ResolveReal();
    uintptr_t key = (uintptr_t)cond;
    LockTable();
    int result=0;auto found=g_conds.find(key);
    if(found!=g_conds.end()){result=g_real.cond_destroy(&found->second->cond);if(result==0)g_conds.erase(found);}
    UnlockTable();
    return result;
}

extern "C" int rea_pthread_rwlock_init(void *lock, const void *attr)
{
    (void)attr;
    ResolveReal();
    LockTable();
    pthread_rwlock_t *real = RwlockFor(lock);
    UnlockTable();
    return real ? 0 : -1;
}

extern "C" int rea_pthread_rwlock_rdlock(void *lock)
{
    ResolveReal();
    LockTable();
    pthread_rwlock_t *real = RwlockFor(lock);
    UnlockTable();
    return real ? g_real.rwlock_rdlock(real) : -1;
}

extern "C" int rea_pthread_rwlock_wrlock(void *lock)
{
    ResolveReal();
    LockTable();
    pthread_rwlock_t *real = RwlockFor(lock);
    UnlockTable();
    return real ? g_real.rwlock_wrlock(real) : -1;
}

extern "C" int rea_pthread_rwlock_unlock(void *lock)
{
    ResolveReal();
    LockTable();
    pthread_rwlock_t *real = RwlockFor(lock);
    UnlockTable();
    return real ? g_real.rwlock_unlock(real) : -1;
}

extern "C" int rea_pthread_rwlock_destroy(void *lock)
{
    ResolveReal();
    uintptr_t key = (uintptr_t)lock;
    LockTable();
    int result=0;auto found=g_rwlocks.find(key);
    if(found!=g_rwlocks.end()){result=g_real.rwlock_destroy(&found->second->lock);if(result==0)g_rwlocks.erase(found);}
    UnlockTable();
    return result;
}

/* ---- loader tracing: shows whether the engine loads libil2cpp itself ---------------------- */

namespace {

void *(*g_realDlopen)(const char *, int) = nullptr;
void *(*g_realAndroidDlopenExt)(const char *, int, const void *) = nullptr;
void *(*g_realDlsym)(void *, const char *) = nullptr;

void ResolveLoader()
{
    if (g_realDlopen) return;
    void *libc = dlopen("libc.so", RTLD_NOLOAD | RTLD_LAZY);
    if (!libc) libc = dlopen("libc.so", RTLD_LAZY);
    if (!libc) return;
    g_realDlopen = (void *(*)(const char *, int))dlsym(libc, "dlopen");
    g_realAndroidDlopenExt = (void *(*)(const char *, int, const void *))dlsym(libc, "android_dlopen_ext");
    g_realDlsym = (void *(*)(void *, const char *))dlsym(libc, "dlsym");
}

} // namespace

extern "C" void *rea_dlopen(const char *name, int flags)
{
    ResolveLoader();

    /*
     * The engine loads libunity and libil2cpp with RTLD_LAZY only, which leaves them in local scope.
     * Their symbols are then invisible to the default lookup, so the engine's 127 il2cpp entry-point
     * resolutions all return null and it later calls through null pointers. Promoting these two loads to
     * global scope fixes the lookups at their source.
     */
    if (name && (strstr(name, "libil2cpp.so") || strstr(name, "libunity.so") ||
                 strstr(name, "libmain.so"))) {
        flags |= RTLD_GLOBAL;
    }

    // Device proof: the NDK GLESv2 dlsym facade has an empty hook table in
    // this Android-hosted player, while the system GLESv3 entry and EGL proc
    // entry both return the real current renderer. Route only foreign loads.
    if (name && strcmp(name, "libGLESv2.so") == 0 && g_realDlopen) {
        void *nativeGles = g_realDlopen("libGLESv3.so", flags);
        if (nativeGles) {
            LOGI("GRAPHICS_LIBRARY_ROUTE GLESv2 -> native GLESv3 handle=%{public}p", nativeGles);
            return nativeGles;
        }
        LOGW("GRAPHICS_LIBRARY_ROUTE native GLESv3 unavailable; keeping original load");
    }
    void *handle = g_realDlopen ? g_realDlopen(name, flags) : nullptr;
    LOGI("dlopen(\"%{public}s\", 0x%{public}x) = %{public}p", name ? name : "(null)", flags, handle);
    return handle;
}

extern "C" void *rea_android_dlopen_ext(const char *name, int flags, const void *extinfo)
{
    ResolveLoader();
    void *handle = g_realAndroidDlopenExt ? g_realAndroidDlopenExt(name, flags, extinfo) : nullptr;
    LOGI("android_dlopen_ext(\"%{public}s\", 0x%{public}x) = %{public}p", name ? name : "(null)", flags,
         handle);
    return handle;
}

extern "C" void *ReaObserveGraphicsSymbol(const char *,void *);
extern "C" void *rea_dlsym(void *handle, const char *name)
{
    ResolveLoader();
    void *result = g_realDlsym ? g_realDlsym(handle, name) : nullptr;

    {
        static int callCount = 0;
        if (callCount < 10) {
            LOGI("dlsym call #%{public}d handle=%{public}p name=%{public}s result=%{public}p", callCount,
                 handle, name ? name : "(null)", result);
            callCount++;
        }
    }

    /*
     * The engine resolves its il2cpp entry points with dlsym against its own library handle. libil2cpp is
     * not a dependency of that library, so all of those lookups return null and the engine later calls
     * through the null pointers. Looking the symbol up in libil2cpp instead fixes it at the source.
     */
    if (!result && name && strncmp(name, "il2cpp", 6) == 0) {
        static void *scriptingHandle = nullptr;
        static int reported = 0;
        const bool trace = reported < 4;

        if (!scriptingHandle && g_realDlopen) {
            /* Try the default scope first, then the short name, then the install path. */
            scriptingHandle = g_realDlopen(nullptr, RTLD_LAZY);
            if (!scriptingHandle) {
                scriptingHandle = g_realDlopen("libil2cpp.so", RTLD_LAZY | RTLD_GLOBAL);
            }
            if (!scriptingHandle) {
                scriptingHandle =
                    g_realDlopen("/data/storage/el1/bundle/libs/arm64/libil2cpp.so",
                                 RTLD_LAZY | RTLD_GLOBAL);
            }
        }

        if (scriptingHandle) {
            result = g_realDlsym(scriptingHandle, name);
        }
        if (trace) {
            LOGI("dlsym fallback: %{public}s handle=%{public}p result=%{public}p", name, scriptingHandle,
                 result);
            reported++;
        }
    }
    return ReaObserveGraphicsSymbol(name,result);
}

/* ---- semaphores: bionic's sem_t is tiny, HarmonyOS's is 32 bytes -------------------------- */

extern "C" int rea_sem_init(void *sem, int pshared, unsigned value)
{
    ResolveReal();
    (void)pshared;
    (void)value;
    LockTable();
    SemEntry *entry = nullptr;
    uintptr_t key = (uintptr_t)sem;
    if (key) {
        uintptr_t slot = (key >> 3) % kTableSize;
        for (int i = 0; i < kTableSize; i++) {
            SemEntry &candidate = g_sems[(slot + i) % kTableSize];
            if (candidate.key == key) { entry = &candidate; break; }
            if(candidate.key==kDeletedSem&&!entry)entry=&candidate;
            if (candidate.key == 0) {
                if(!entry)entry = &candidate;
                break;
            }
        }
    }
    int result=-1;
    if (entry && g_real.sem_init) {
        result=g_real.sem_init(&entry->sem,0,value);
        if (!result) {
            entry->posts=entry->waits=0;
            __atomic_store_n(&entry->key,key,__ATOMIC_RELEASE);
        }
    }
    UnlockTable();
    return result;
}

extern "C" int rea_sem_wait(void *sem)
{
    ResolveReal();
    LockTable();
    sem_t *real = SemFor(sem);
    UnlockTable();
    int result=(real && g_real.sem_wait) ? g_real.sem_wait(real) : -1;
    if (!result) __atomic_fetch_add(&reinterpret_cast<SemEntry *>((char *)real - offsetof(SemEntry,sem))->waits,1UL,__ATOMIC_RELAXED);
    return result;
}

extern "C" int rea_sem_trywait(void *sem)
{
    ResolveReal();
    LockTable();
    sem_t *real = SemFor(sem);
    UnlockTable();
    int result=(real && g_real.sem_trywait) ? g_real.sem_trywait(real) : -1;
    if (!result)__atomic_fetch_add(&reinterpret_cast<SemEntry *>((char *)real - offsetof(SemEntry,sem))->waits,1UL,__ATOMIC_RELAXED);
    return result;
}

extern "C" int rea_sem_timedwait(void *sem, const struct timespec *abstime)
{
    ResolveReal();
    LockTable();
    sem_t *real = SemFor(sem);
    UnlockTable();
    int result=(real && g_real.sem_timedwait) ? g_real.sem_timedwait(real, abstime) : -1;
    if (!result)__atomic_fetch_add(&reinterpret_cast<SemEntry *>((char *)real - offsetof(SemEntry,sem))->waits,1UL,__ATOMIC_RELAXED);
    return result;
}

extern "C" int rea_sem_post(void *sem)
{
    // POSIX sem_post is async-signal-safe. The IL2CPP suspend handler calls it.
    // Do not resolve libraries, allocate TLS, or acquire our mapping-table lock.
    // sem_init publishes the stable native object before publishing its key.
    uintptr_t key=(uintptr_t)sem;
    if (key && g_real.sem_post) {
        uintptr_t slot=(key>>3)%kTableSize;
        for (int i=0;i<kTableSize;++i) {
            SemEntry &entry=g_sems[(slot+i)%kTableSize];
            if (__atomic_load_n(&entry.key,__ATOMIC_ACQUIRE)!=key) continue;
            int result=g_real.sem_post(&entry.sem);
            if (!result)__atomic_fetch_add(&entry.posts,1UL,__ATOMIC_RELAXED);
            return result;
        }
    }
    errno=EINVAL;return -1;
}

extern "C" void ReaDumpThreadAbi()
{
    LOGI("GC_ABI sends_suspend=%{public}lu sends_restart=%{public}lu send_failed=%{public}lu table_owner=%{public}d",
         __atomic_load_n(&g_suspendSends,__ATOMIC_RELAXED),__atomic_load_n(&g_restartSends,__ATOMIC_RELAXED),
         __atomic_load_n(&g_signalSendFailures,__ATOMIC_RELAXED),__atomic_load_n(&g_tableOwnerTid,__ATOMIC_ACQUIRE));
    struct Snapshot {uintptr_t key;int value;unsigned long posts,waits;};
    Snapshot records[48];int count=0;
    LockTable();
    for (auto &entry:g_sems) if (entry.key && entry.key!=kDeletedSem && count<48) {
        int value=-999;g_real.sem_getvalue(&entry.sem,&value);
        records[count++]={entry.key,value,__atomic_load_n(&entry.posts,__ATOMIC_RELAXED),__atomic_load_n(&entry.waits,__ATOMIC_RELAXED)};
    }
    UnlockTable();
    for (int i=0;i<count;++i) {
        Dl_info info{};dladdr((void *)records[i].key,&info);
        LOGI("GC_SEM key=%{public}p module=%{public}s+0x%{public}lx value=%{public}d posts=%{public}lu waits=%{public}lu",(void *)records[i].key,
             info.dli_fname?info.dli_fname:"heap",info.dli_fbase?records[i].key-(uintptr_t)info.dli_fbase:0,
             records[i].value,records[i].posts,records[i].waits);
    }
}

extern "C" void ReaLogSyncState(){
    size_t mutexes,conditions,rwlocks,mutexPeak,condPeak,rwPeak,semaphores=0;
    LockTable();mutexes=g_mutexes.size();conditions=g_conds.size();rwlocks=g_rwlocks.size();
    mutexPeak=g_mutexPeak;condPeak=g_condPeak;rwPeak=g_rwPeak;
    for(const auto &entry:g_sems)if(entry.key&&entry.key!=kDeletedSem)++semaphores;
    UnlockTable();
    LOGI("SYNC_REGISTRY mutex=%{public}zu cond=%{public}zu rw=%{public}zu sem=%{public}zu peaks=%{public}zu/%{public}zu/%{public}zu growable=1",
         mutexes,conditions,rwlocks,semaphores,mutexPeak,condPeak,rwPeak);
}

extern "C" int rea_sem_getvalue(void *sem, int *value)
{
    ResolveReal();
    LockTable();
    sem_t *real = SemFor(sem);
    UnlockTable();
    return (real && g_real.sem_getvalue) ? g_real.sem_getvalue(real, value) : -1;
}

extern "C" int rea_sem_destroy(void *sem)
{
    ResolveReal();
    uintptr_t key = (uintptr_t)sem;
    LockTable();
    uintptr_t slot = (key >> 3) % kTableSize;
    for (int i = 0; i < kTableSize; i++) {
        SemEntry &entry = g_sems[(slot + i) % kTableSize];
        if (entry.key == key) {
            if (g_real.sem_destroy) g_real.sem_destroy(&entry.sem);
            __atomic_store_n(&entry.key,kDeletedSem,__ATOMIC_RELEASE);
            break;
        }
        if (entry.key == 0) break;
    }
    UnlockTable();
    return 0;
}

/* ---- scripting runtime entry points: report initialisation outcomes ---------------------- */

namespace {

int (*g_realIl2cppInit)(const char *) = nullptr;
void *(*g_realIl2cppDomainGet)() = nullptr;

} // namespace

extern "C" void ReaSetRealIl2cppInit(void *init, void *domainGet)
{
    g_realIl2cppInit = (int (*)(const char *))init;
    g_realIl2cppDomainGet = (void *(*)())domainGet;
}

extern "C" int rea_il2cpp_init(const char *domainName)
{
    int result = g_realIl2cppInit ? g_realIl2cppInit(domainName) : -1;
    LOGI("il2cpp_init(\"%{public}s\") = %{public}d", domainName ? domainName : "(null)", result);
    return result;
}

void *(*g_realClassFromName)(void *, const char *, const char *) = nullptr;
void *(*g_realClassGetMethod)(void *, const char *, int) = nullptr;

extern "C" void ReaSetRealIl2cppLookups(void *classFromName, void *classGetMethod)
{
    g_realClassFromName = (void *(*)(void *, const char *, const char *))classFromName;
    g_realClassGetMethod = (void *(*)(void *, const char *, int))classGetMethod;
}

extern "C" void *rea_il2cpp_class_from_name(void *image, const char *nameSpace, const char *name)
{
    void *klass = g_realClassFromName ? g_realClassFromName(image, nameSpace, name) : nullptr;
    static int reported = 0;
    if (!klass && reported < 6) {
        LOGI("il2cpp_class_from_name(%{public}s::%{public}s) = null", nameSpace ? nameSpace : "",
             name ? name : "");
        reported++;
    }
    return klass;
}

extern "C" void *rea_il2cpp_class_get_method_from_name(void *klass, const char *name, int argc)
{
    void *method = g_realClassGetMethod ? g_realClassGetMethod(klass, name, argc) : nullptr;
    static int reported = 0;
    if (!method && reported < 6) {
        LOGI("il2cpp_class_get_method_from_name(klass=%{public}p, %{public}s, %{public}d) = null", klass,
             name ? name : "", argc);
        reported++;
    }
    return method;
}

extern "C" void *rea_il2cpp_domain_get()
{
    void *domain = g_realIl2cppDomainGet ? g_realIl2cppDomainGet() : nullptr;
    LOGI("il2cpp_domain_get() = %{public}p", domain);
    return domain;
}

/* ---- signal sets: 8-byte bionic objects vs 128-byte HarmonyOS ones ------------------------ */

/*
 * Layout the Android toolchain uses. Fixed here rather than taken from headers because this
 * translation unit is compiled for HarmonyOS, whose struct is the large one.
 */
// Android bionic LP64 places flags FIRST; the ILP32 field order is different.
// Source: AOSP libc/include/bits/signal_types.h __LP64__ branch.
struct BionicSigaction {
    int flags;
    union {
        void (*handler)(int);
        void (*sigaction_handler)(int, void *, void *);
    } action;
    unsigned long mask[1];
    void (*restorer)(void);
};
static_assert(sizeof(BionicSigaction)==32 && offsetof(BionicSigaction,flags)==0 &&
    offsetof(BionicSigaction,action)==8 && offsetof(BionicSigaction,mask)==16 &&
    offsetof(BionicSigaction,restorer)==24,"Android LP64 sigaction ABI");

extern "C" int rea_sigemptyset(void *set)
{
    if (!set) return -1;
    /* bionic semantics: clear exactly its 8-byte set, never touch neighbouring memory. */
    memset(set, 0, sizeof(unsigned long));
    return 0;
}

extern "C" int rea_sigfillset(void *set)
{
    if (!set) {errno=EINVAL;return -1;}
    unsigned long bits=~0UL;memcpy(set,&bits,8);return 0;
}
extern "C" int rea_sigaddset(void *set,int signalNumber)
{
    if (!set || signalNumber<1 || signalNumber>64) {errno=EINVAL;return -1;}
    unsigned long bits;memcpy(&bits,set,8);bits|=1UL<<(signalNumber-1);memcpy(set,&bits,8);return 0;
}
extern "C" int rea_sigdelset(void *set,int signalNumber)
{
    if (!set || signalNumber<1 || signalNumber>64) {errno=EINVAL;return -1;}
    unsigned long bits;memcpy(&bits,set,8);bits&=~(1UL<<(signalNumber-1));memcpy(set,&bits,8);return 0;
}
static int TranslateSignalMask(bool thread,int how,const void *set,void *oldset)
{
    sigset_t nativeSet{},nativeOld{};
    if (set)memcpy(&nativeSet,set,8);
    int result=thread ? pthread_sigmask(how,set?&nativeSet:nullptr,oldset?&nativeOld:nullptr)
                      : sigprocmask(how,set?&nativeSet:nullptr,oldset?&nativeOld:nullptr);
    if (result==0 && oldset)memcpy(oldset,&nativeOld,8);
    return result;
}
extern "C" int rea_pthread_sigmask(int how,const void *set,void *oldset)
{return TranslateSignalMask(true,how,set,oldset);}
extern "C" int rea_sigprocmask(int how,const void *set,void *oldset)
{return TranslateSignalMask(false,how,set,oldset);}
extern "C" int rea_sigsuspend(const void *set)
{
    if (!set) {errno=EINVAL;return -1;}
    sigset_t nativeSet{};memcpy(&nativeSet,set,8);return sigsuspend(&nativeSet);
}

extern "C" int rea_sigaction(int signum, const void *act, void *oldact)
{
    ResolveReal();
    if (!g_real.sigaction) return -1;

    struct sigaction realAct;
    struct sigaction realOld;
    memset(&realAct, 0, sizeof(realAct));
    memset(&realOld, 0, sizeof(realOld));

    if (act) {
        const BionicSigaction *small = (const BionicSigaction *)act;
        memcpy(&realAct.__sa_handler, &small->action, sizeof(realAct.__sa_handler));
        memcpy(realAct.sa_mask.__bits, small->mask, sizeof(small->mask));
        realAct.sa_flags = small->flags;
        realAct.sa_restorer = small->restorer;
        if(signum==SIGPWR || signum==SIGXCPU || signum==SIGSEGV)
            LOGI("SIGACTION_LP64 signal=%{public}d handler=%{public}p flags=%{public}x mask=%{public}lx",signum,reinterpret_cast<void*>(small->action.handler),small->flags,small->mask[0]);
    }

    int result = g_real.sigaction(signum, act ? &realAct : nullptr, oldact ? &realOld : nullptr);
    if (result != 0) return result;

    if (oldact) {
        BionicSigaction *small = (BionicSigaction *)oldact;
        memset(small, 0, sizeof(*small));
        memcpy(&small->action, &realOld.__sa_handler, sizeof(small->action));
        memcpy(small->mask, realOld.sa_mask.__bits, sizeof(small->mask));
        small->flags = realOld.sa_flags;
        small->restorer = realOld.sa_restorer;
    }
    return result;
}

/* ---------------------------------------------------------------------------------------------
 * GOT rewriting
 *
 * libunity's PLT slots are already resolved to libc by the time it is loaded, so exporting these
 * names from a shim library cannot win the lookup. Rewriting the slots of that one library redirects
 * exactly its pthread calls.
 * ------------------------------------------------------------------------------------------- */

namespace {

struct PatchStat {
    int mutex;
    int cond;
    int rejected;
    bool relroUnlocked;
};

bool MakeWritable(void *page, size_t length)
{
    long pageSize = 4096;
    uintptr_t start = (uintptr_t)page & ~(uintptr_t)(pageSize - 1);
    uintptr_t end = ((uintptr_t)page + length + pageSize - 1) & ~(uintptr_t)(pageSize - 1);
    int result = mprotect((void *)start, end - start, PROT_READ | PROT_WRITE);
    return result == 0;
}

} // namespace

/*
 * Rewrites libunity's pthread PLT slots. Returns the number of slots redirected.
 * The caller passes the library base obtained from dladdr on one of its symbols.
 */
/* Hands the engine the OHNativeWindow directly, bypassing the platform library's own check. */
extern "C" void *ReaShim_GetNativeWindow(void);
extern "C" bool ReaShim_JavaSurfaceWindow(void *,void **);

extern "C" int ReaGetRenderWindowSize(int32_t *,int32_t *);
extern "C" int32_t rea_ANativeWindow_getWidth(void *window)
{
    int32_t width=0,height=0;
    if(window&&OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(window),GET_BUFFER_GEOMETRY,&height,&width)==0&&width>0)return width;
    ReaGetRenderWindowSize(&width,&height);return width;
}
extern "C" int32_t rea_ANativeWindow_getHeight(void *window)
{
    int32_t width=0,height=0;
    if(window&&OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(window),GET_BUFFER_GEOMETRY,&height,&width)==0&&height>0)return height;
    ReaGetRenderWindowSize(&width,&height);return height;
}
extern "C" void *rea_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env;
    void *window=nullptr;
    if(ReaShim_JavaSurfaceWindow(surface,&window)){
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"ReaMediaShim","MEDIA_SURFACE_ROUTE mapped=1 window=%{public}p",window);return window;
    }
    window = ReaShim_GetNativeWindow();
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "TransPthread",
                 "ANativeWindow_fromSurface interposed -> %{public}p", window);
    return window;
}

extern "C" int ReaPatchPthreadGOT(void *imageBase)
{
    if (!imageBase) return 0;
    ResolveReal();

    unsigned char *base = (unsigned char *)imageBase;
    if (!(base[0] == 0x7f && base[1] == 'E' && base[2] == 'L' && base[3] == 'F')) {
        LOGW("image base is not an ELF header");
        return 0;
    }

    /* Program headers are always mapped, unlike the section header table. */
    unsigned long long phoff = 0;
    unsigned short phentsize = 0, phnum = 0;
    memcpy(&phoff, base + 0x20, 8);
    memcpy(&phentsize, base + 0x36, 2);
    memcpy(&phnum, base + 0x38, 2);
    if (!phoff || !phentsize || !phnum) {
        LOGW("no program headers");
        return 0;
    }

    unsigned long long dynamicVaddr = 0, dynamicSize = 0;
    for (int i = 0; i < phnum; i++) {
        unsigned char *ph = base + phoff + (unsigned long long)i * phentsize;
        unsigned int type = 0;
        unsigned long long vaddr = 0, memsz = 0;
        memcpy(&type, ph, 4);
        memcpy(&vaddr, ph + 0x10, 8);
        memcpy(&memsz, ph + 0x28, 8);
        if (type == 2 /* PT_DYNAMIC */) {
            dynamicVaddr = vaddr;
            dynamicSize = memsz;
            break;
        }
    }
    if (!dynamicVaddr) {
        LOGW("PT_DYNAMIC not found");
        return 0;
    }

    /* Dynamic entries: tag/value pairs. Pointer values may be link-time addresses. */
    unsigned long long jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0;
    unsigned long long *dynamic = (unsigned long long *)(base + dynamicVaddr);
    for (unsigned long long i = 0; i < dynamicSize / 16; i++) {
        unsigned long long tag = dynamic[i * 2];
        unsigned long long value = dynamic[i * 2 + 1];
        if (tag == 0) break;
        switch (tag) {
            case 23 /* DT_JMPREL */: jmprel = value; break;
            case 2  /* DT_PLTRELSZ */: pltrelsz = value; break;
            case 6  /* DT_SYMTAB */: symtab = value; break;
            case 5  /* DT_STRTAB */: strtab = value; break;
            default: break;
        }
    }
    if (!jmprel || !pltrelsz || !symtab || !strtab) {
        LOGW("dynamic entries incomplete: jmprel=%llu pltrelsz=%llu symtab=%llu strtab=%llu",
             jmprel, pltrelsz, symtab, strtab);
        return 0;
    }

    /* A value smaller than the base is a link-time address that needs the base added. */
    auto resolve = [base](unsigned long long value) -> unsigned char * {
        unsigned long long baseValue = (unsigned long long)(uintptr_t)base;
        if (value < baseValue) return (unsigned char *)(baseValue + value);
        return (unsigned char *)value;
    };

    unsigned char *relaTable = resolve(jmprel);
    unsigned char *symTable = resolve(symtab);
    const char *strTable = (const char *)resolve(strtab);

    struct PatchTable {
        const char *name;
        void *replacement;
    } table[] = {
        {"__cxa_throw", ReaExceptionThrowHook()},
        {"pthread_mutex_init", (void *)&rea_pthread_mutex_init},
        {"pthread_mutex_lock", (void *)&rea_pthread_mutex_lock},
        {"pthread_mutex_trylock", (void *)&rea_pthread_mutex_trylock},
        {"pthread_mutex_unlock", (void *)&rea_pthread_mutex_unlock},
        {"pthread_mutex_destroy", (void *)&rea_pthread_mutex_destroy},
        {"pthread_cond_init", (void *)&rea_pthread_cond_init},
        {"pthread_cond_wait", (void *)&rea_pthread_cond_wait},
        {"pthread_cond_timedwait", (void *)&rea_pthread_cond_timedwait},
        {"pthread_create", (void *)&rea_pthread_create},
        {"pthread_kill", (void *)&rea_pthread_kill},
        {"ANativeWindow_fromSurface", (void *)&rea_ANativeWindow_fromSurface},
        {"ANativeWindow_getWidth", (void *)&rea_ANativeWindow_getWidth},
        {"ANativeWindow_getHeight", (void *)&rea_ANativeWindow_getHeight},
    {"pthread_cond_signal", (void *)&rea_pthread_cond_signal},
        {"pthread_cond_broadcast", (void *)&rea_pthread_cond_broadcast},
        {"pthread_cond_destroy", (void *)&rea_pthread_cond_destroy},
        {"pthread_rwlock_init", (void *)&rea_pthread_rwlock_init},
        {"pthread_rwlock_rdlock", (void *)&rea_pthread_rwlock_rdlock},
        {"pthread_rwlock_wrlock", (void *)&rea_pthread_rwlock_wrlock},
        {"pthread_rwlock_unlock", (void *)&rea_pthread_rwlock_unlock},
        {"pthread_rwlock_destroy", (void *)&rea_pthread_rwlock_destroy},
        {"sigemptyset", (void *)&rea_sigemptyset},
        {"sigfillset", (void *)&rea_sigfillset},
        {"sigaddset", (void *)&rea_sigaddset},
        {"sigdelset", (void *)&rea_sigdelset},
        {"pthread_sigmask", (void *)&rea_pthread_sigmask},
        {"sigprocmask", (void *)&rea_sigprocmask},
        {"sigsuspend", (void *)&rea_sigsuspend},
        {"sigaction", (void *)&rea_sigaction},
        {"sem_init", (void *)&rea_sem_init},
        {"sem_wait", (void *)&rea_sem_wait},
        {"sem_trywait", (void *)&rea_sem_trywait},
        {"sem_timedwait", (void *)&rea_sem_timedwait},
        {"sem_post", (void *)&rea_sem_post},
        {"sem_getvalue", (void *)&rea_sem_getvalue},
        {"sem_destroy", (void *)&rea_sem_destroy},
        {"dlopen", (void *)&rea_dlopen},
        {"android_dlopen_ext", (void *)&rea_android_dlopen_ext},
        {"dlsym", (void *)&rea_dlsym},
        {"getaddrinfo", (void *)&rea_getaddrinfo},
        {"freeaddrinfo", (void *)&rea_freeaddrinfo},
        {"gai_strerror", (void *)&rea_gai_strerror},
        {"getnameinfo", (void *)&rea_getnameinfo},
        {"connect", (void *)&rea_net_connect},
        {"sysconf", (void *)&rea_sysconf},
    };

    int patchedMutex = 0;
    int patchedCond = 0;
    int patchedOther = 0;
    int rejected = 0;
    /* Remember one patched slot so the write can be read back afterwards. */
    void **firstSlot = nullptr;
    void *firstExpected = nullptr;
    const char *firstName = nullptr;
    unsigned long long count = pltrelsz / 24;
    for (unsigned long long i = 0; i < count; i++) {
        unsigned char *entry = relaTable + i * 24;
        unsigned long long r_offset = 0, r_info = 0;
        memcpy(&r_offset, entry, 8);
        memcpy(&r_info, entry + 8, 8);
        unsigned int symIndex = (unsigned int)(r_info >> 32);
        if (!symIndex) continue;

        unsigned char *sym = symTable + (unsigned long long)symIndex * 24;
        unsigned int nameOff = 0;
        memcpy(&nameOff, sym, 4);
        const char *name = strTable + nameOff;

        for (const auto &candidate : table) {
            if (strcmp(name, candidate.name) != 0 || !candidate.replacement) continue;
            /* r_offset is a link-time address of the GOT slot. */
            void **slot = (void **)(base + r_offset);
            if (!MakeWritable(slot, sizeof(void *))) {
                rejected++;
                break;
            }
            *slot = candidate.replacement;
            if (!firstSlot) {
                firstSlot = slot;
                firstExpected = candidate.replacement;
                firstName = candidate.name;
            }
            if (strncmp(name, "pthread_mutex", 13) == 0) patchedMutex++;
            else if (strncmp(name, "pthread_cond", 12) == 0) patchedCond++;
            else patchedOther++;
            break;
        }
    }

    LOGI("pthread GOT patched: mutex=%d cond=%d other=%d rejected=%d",
         patchedMutex, patchedCond, patchedOther, rejected);

    /* Read back one slot: a slot that refused the write would fake a failed fix. */
    if (firstSlot) {
        Dl_info info;
        memset(&info, 0, sizeof(info));
        const char *module = "?";
        unsigned long offset = 0;
        void *value = *firstSlot;
        if (value && dladdr(value, &info) && info.dli_fname) {
            module = info.dli_fname;
            offset = (unsigned long)(uintptr_t)value - (unsigned long)(uintptr_t)info.dli_fbase;
        }
        LOGI("GOT verify %s: slot=%p value=%p (%s+0x%lx) expected=%p",
             firstName ? firstName : "?", (void *)firstSlot, value, module, offset, firstExpected);
    } else {
        LOGW("GOT verify: nothing was patched");
    }

    return patchedMutex + patchedCond + patchedOther;
}
/* ---- self-test: prove the translations never write past a bionic-sized object -------------- */

namespace {

const unsigned char kCanary = 0xA5;
const int kCanarySize = 64;

struct GuardedObject {
    unsigned long object;                 /* the foreign (bionic-sized) storage */
    unsigned char canary[kCanarySize];
};

bool CanaryIntact(const GuardedObject &guarded)
{
    for (int i = 0; i < kCanarySize; i++) {
        if (guarded.canary[i] != kCanary) return false;
    }
    return true;
}

void ResetGuarded(GuardedObject *guarded)
{
    memset(guarded, kCanary, sizeof(*guarded));
    guarded->object = 0;
}

} // namespace

extern "C" void ReaStartCondRescue(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, CondRescue, nullptr) == 0) pthread_detach(thread);
}

static volatile sig_atomic_t g_signalContractCount=0;
static void SignalContractHandler(int sig,siginfo_t *info,void *context)
{
    if(sig==SIGUSR2 && info && context)g_signalContractCount=1;
}
static volatile sig_atomic_t g_criticalSignalCount=0;
static void *volatile g_criticalSignalSem=nullptr;
static void CriticalSignalHandler(int sig)
{
    if (sig==SIGPWR || sig==SIGUSR2) {
        ++g_criticalSignalCount;
        if (g_criticalSignalSem) rea_sem_post(g_criticalSignalSem);
    }
}

extern "C" const char *ReaAbiSelfTest()
{
    char report[2048];
    int offset = 0;
    int passed = 0;
    int failed = 0;

    ResolveReal();

    offset += snprintf(report + offset, sizeof(report) - offset,
                       "real api resolved=%d\n", g_real.ready ? 1 : 0);
    bool networkOkay=ReaNetworkSelfTest()==0;
    offset+=snprintf(report+offset,sizeof(report)-offset,"network LP64 ABI contract: %s\n",networkOkay?"PASS":"FAIL");
    networkOkay?passed++:failed++;
    bool systemOkay=ReaSysconfSelfTest()==0;
    offset+=snprintf(report+offset,sizeof(report)-offset,"sysconf symbolic ABI contract: %s\n",systemOkay?"PASS":"FAIL");
    systemOkay?passed++:failed++;

    /* 1. sigemptyset on an 8-byte set must not touch the canary. */
    {
        GuardedObject guarded;
        ResetGuarded(&guarded);
        rea_sigemptyset(&guarded.object);
        bool ok = CanaryIntact(guarded) && guarded.object == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "sigemptyset 8B object: %s\n", ok ? "PASS" : "FAIL");
        ok ? passed++ : failed++;
    }

    /* 2. mutex lock/unlock through the translation, on a 4-byte word. */
    {
        GuardedObject guarded;
        ResetGuarded(&guarded);
        int rc1 = rea_pthread_mutex_init(&guarded.object, nullptr);
        int rc2 = rea_pthread_mutex_lock(&guarded.object);
        int rc3 = rea_pthread_mutex_unlock(&guarded.object);
        int rc4 = rea_pthread_mutex_destroy(&guarded.object);
        bool ok = CanaryIntact(guarded) && rc1 == 0 && rc2 == 0 && rc3 == 0 && rc4 == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "mutex 4B object: %s (rc=%d,%d,%d,%d)\n",
                           ok ? "PASS" : "FAIL", rc1, rc2, rc3, rc4);
        ok ? passed++ : failed++;
    }

    /* 3. recursive mutex must allow relocking, which is what the engine's locks expect. */
    {
        GuardedObject guarded;
        ResetGuarded(&guarded);
        rea_pthread_mutex_lock(&guarded.object);
        int rc = rea_pthread_mutex_lock(&guarded.object);
        rea_pthread_mutex_unlock(&guarded.object);
        rea_pthread_mutex_unlock(&guarded.object);
        rea_pthread_mutex_destroy(&guarded.object);
        bool ok = CanaryIntact(guarded) && rc == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "recursive relock: %s\n", ok ? "PASS" : "FAIL");
        ok ? passed++ : failed++;
    }

    /* 4. semaphore through the translation. */
    {
        GuardedObject guarded;
        ResetGuarded(&guarded);
        rea_sem_init(&guarded.object, 0, 1);
        int rc1 = rea_sem_wait(&guarded.object);
        int rc2 = rea_sem_post(&guarded.object);
        int value = -1;
        rea_sem_getvalue(&guarded.object, &value);
        rea_sem_destroy(&guarded.object);
        bool ok = CanaryIntact(guarded) && rc1 == 0 && rc2 == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "semaphore 4B object: %s (rc=%d,%d value=%d)\n",
                           ok ? "PASS" : "FAIL", rc1, rc2, value);
        ok ? passed++ : failed++;
    }

    /* 5. sigaction write-back: the engine passes a stack oldact, which is where the 152-byte
     * HarmonyOS struct used to overwrite the saved return address. */
    {
        struct {
            unsigned char oldact[32];   /* bionic-sized struct sigaction */
            unsigned char canary[64];
        } probe;
        memset(probe.oldact, 0, sizeof(probe.oldact));
        memset(probe.canary, kCanary, sizeof(probe.canary));
        int rc = rea_sigaction(SIGCONT, nullptr, probe.oldact);
        bool canaryOk = true;
        for (int i = 0; i < 64; i++) {
            if (probe.canary[i] != kCanary) canaryOk = false;
        }
        bool ok = canaryOk && rc == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "sigaction oldact 32B: %s (rc=%d)\n", ok ? "PASS" : "FAIL", rc);
        ok ? passed++ : failed++;
    }

    /* 6. sigaltstack round-trip: stack_t layouts agree, so this should pass untouched. */
    {
        struct {
            stack_t query;
            unsigned char canary[64];
        } probe;
        memset(&probe.query, 0, sizeof(probe.query));
        memset(probe.canary, kCanary, sizeof(probe.canary));
        int rc = sigaltstack(nullptr, &probe.query);
        bool canaryOk = true;
        for (int i = 0; i < 64; i++) {
            if (probe.canary[i] != kCanary) canaryOk = false;
        }
        bool ok = canaryOk && rc == 0;
        offset += snprintf(report + offset, sizeof(report) - offset,
                           "sigaltstack query: %s (rc=%d)\n", ok ? "PASS" : "FAIL", rc);
        ok ? passed++ : failed++;
    }

    /* Raw-byte contract independent of the translating struct: LP64 flags0,
     * handler8, mask16, restorer24. Verify native state BEFORE sending a signal,
     * so a bad adapter produces FAIL rather than jumping through a bad handler. */
    {
        struct sigaction previous{},observed{};
        bool saved=g_real.sigaction(SIGUSR2,nullptr,&previous)==0;
        unsigned char foreign[32];memset(foreign,0xA5,sizeof(foreign));
        int flags=SA_SIGINFO;memcpy(foreign,&flags,sizeof(flags));
        auto handler=&SignalContractHandler;memcpy(foreign+8,&handler,sizeof(handler));
        unsigned long mask=1UL<<(SIGINT-1);memcpy(foreign+16,&mask,sizeof(mask));
        void (*restorer)()=nullptr;memcpy(foreign+24,&restorer,sizeof(restorer));
        int rc=saved?rea_sigaction(SIGUSR2,foreign,nullptr):-1;
        bool native=saved&&rc==0&&g_real.sigaction(SIGUSR2,nullptr,&observed)==0&&
            observed.sa_sigaction==handler&&(observed.sa_flags&SA_SIGINFO)&&sigismember(&observed.sa_mask,SIGINT)==1;
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigaction LP64 fields to native: %s\n",native?"PASS":"FAIL");native?passed++:failed++;
        struct {unsigned char bytes[32];unsigned char canary[64];} back;
        memset(&back,0xA5,sizeof(back));
        rc=saved?rea_sigaction(SIGUSR2,nullptr,back.bytes):-1;
        int readFlags=0;void *readHandler=nullptr;unsigned long readMask=0;
        memcpy(&readFlags,back.bytes,4);memcpy(&readHandler,back.bytes+8,8);memcpy(&readMask,back.bytes+16,8);
        bool canary=true;for(unsigned char value:back.canary)if(value!=0xA5)canary=false;
        bool reverse=native&&rc==0&&(readFlags&SA_SIGINFO)&&readHandler==reinterpret_cast<void*>(handler)&&(readMask&mask)&&canary;
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigaction LP64 native to fields: %s\n",reverse?"PASS":"FAIL");reverse?passed++:failed++;
        g_signalContractCount=0;
        bool delivered=native&&raise(SIGUSR2)==0&&g_signalContractCount==1;
        if(saved)g_real.sigaction(SIGUSR2,&previous,nullptr);
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigaction real SA_SIGINFO delivery: %s\n",delivered?"PASS":"FAIL");delivered?passed++:failed++;
    }

    {
        struct {unsigned long bits;unsigned char canary[32];} foreign;
        auto intact=[&](){for(auto v:foreign.canary)if(v!=0xA5)return false;return true;};
        memset(&foreign,0xA5,sizeof(foreign));
        bool okay=rea_sigfillset(&foreign.bits)==0 && foreign.bits==~0UL && intact();
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigfillset 8B boundary: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
        okay=rea_sigdelset(&foreign.bits,SIGPWR)==0 && !(foreign.bits&(1UL<<(SIGPWR-1))) &&
             rea_sigaddset(&foreign.bits,SIGPWR)==0 && foreign.bits==~0UL && intact();
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigaddset/sigdelset 8B boundary: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
        sigset_t native{};pthread_sigmask(SIG_SETMASK,nullptr,&native);
        okay=rea_pthread_sigmask(SIG_SETMASK,nullptr,&foreign.bits)==0 && !memcmp(&foreign.bits,&native,8) && intact();
        offset+=snprintf(report+offset,sizeof(report)-offset,"pthread_sigmask oldset 8B: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
        okay=rea_sigprocmask(SIG_SETMASK,nullptr,&foreign.bits)==0 && !memcmp(&foreign.bits,&native,8) && intact();
        offset+=snprintf(report+offset,sizeof(report)-offset,"sigprocmask oldset 8B: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }

    /* Exercise the exact GC pattern: suspend signal arrives while the table is
     * locked, then its handler posts a translated semaphore after unlock. */
    {
        struct sigaction previous{},action{};
        sigset_t mask,savedMask,observedMask;
        sigemptyset(&mask); sigaddset(&mask,SIGPWR);
        bool saved=pthread_sigmask(SIG_UNBLOCK,&mask,&savedMask)==0 && g_real.sigaction(SIGPWR,nullptr,&previous)==0;
        GuardedObject guarded; ResetGuarded(&guarded);
        bool ready=rea_sem_init(&guarded.object,0,0)==0;
        action.sa_handler=CriticalSignalHandler;sigemptyset(&action.sa_mask);
        bool installed=saved&&g_real.sigaction(SIGPWR,&action,nullptr)==0;
        g_criticalSignalCount=0;g_criticalSignalSem=&guarded.object;
        bool deferred=false,delivered=false;
        if (installed&&ready) {
            LockTable(); int rc=raise(SIGPWR); deferred=rc==0&&g_criticalSignalCount==0;
            UnlockTable(); delivered=g_criticalSignalCount==1&&rea_sem_trywait(&guarded.object)==0;
        }
        g_criticalSignalSem=nullptr;
        if (saved)g_real.sigaction(SIGPWR,&previous,nullptr);
        if (ready)rea_sem_destroy(&guarded.object);
        bool okay=deferred&&delivered&&CanaryIntact(guarded);
        offset+=snprintf(report+offset,sizeof(report)-offset,"GC signal deferred, handler sem_post: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
        pthread_sigmask(SIG_BLOCK,&mask,nullptr);
        LockTable();UnlockTable();pthread_sigmask(SIG_SETMASK,nullptr,&observedMask);
        okay=sigismember(&observedMask,SIGPWR)==1;
        if (saved)pthread_sigmask(SIG_SETMASK,&savedMask,nullptr);
        offset+=snprintf(report+offset,sizeof(report)-offset,"GC critical section preserves signal mask: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }

    {
        GuardedObject guarded;ResetGuarded(&guarded);
        bool ready=rea_sem_init(&guarded.object,0,0)==0;
        struct sigaction previous{},action{};
        bool saved=g_real.sigaction(SIGUSR2,nullptr,&previous)==0;
        action.sa_handler=CriticalSignalHandler;sigemptyset(&action.sa_mask);
        bool installed=saved&&g_real.sigaction(SIGUSR2,&action,nullptr)==0;
        g_criticalSignalCount=0;g_criticalSignalSem=&guarded.object;
        bool postedInside=false;
        if (installed&&ready) {
            LockTable();
            postedInside=raise(SIGUSR2)==0 && g_criticalSignalCount==1;
            UnlockTable();
        }
        bool okay=postedInside&&rea_sem_trywait(&guarded.object)==0&&CanaryIntact(guarded);
        g_criticalSignalSem=nullptr;
        if (saved)g_real.sigaction(SIGUSR2,&previous,nullptr);
        if (ready)rea_sem_destroy(&guarded.object);
        offset+=snprintf(report+offset,sizeof(report)-offset,"sem_post in signal with table locked: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }

    {
        // More simultaneously live objects than the old pool's entire capacity.
        std::vector<uint64_t> mutexes(4096),conditions(1536),rwlocks(1536);
        bool okay=true;
        for(auto &key:mutexes)if(rea_pthread_mutex_init(&key,nullptr))okay=false;
        for(auto &key:conditions)if(rea_pthread_cond_init(&key,nullptr))okay=false;
        for(auto &key:rwlocks)if(rea_pthread_rwlock_init(&key,nullptr))okay=false;
        for(auto &key:mutexes){if(rea_pthread_mutex_lock(&key)||rea_pthread_mutex_unlock(&key))okay=false;}
        for(auto &key:mutexes)if(rea_pthread_mutex_destroy(&key))okay=false;
        for(auto &key:conditions)if(rea_pthread_cond_destroy(&key))okay=false;
        for(auto &key:rwlocks)if(rea_pthread_rwlock_destroy(&key))okay=false;
        offset+=snprintf(report+offset,sizeof(report)-offset,"growable locks 4096/1536/1536 live: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }
    {
        // These two foreign addresses collided in every old 1024-slot table.
        std::vector<uint64_t> slots(kTableSize+1);void *first=&slots[0],*second=&slots[kTableSize];
        bool okay=rea_pthread_mutex_init(first,nullptr)==0&&rea_pthread_mutex_init(second,nullptr)==0;
        LockTable();auto *original=MutexFor(second);UnlockTable();rea_pthread_mutex_destroy(first);
        LockTable();okay=okay&&MutexFor(second)==original;UnlockTable();
        okay=okay&&rea_pthread_mutex_lock(second)==0;
        struct Probe {void *foreign;int result;};Probe probe{second,-1};pthread_t worker{};
        auto tryOther=[](void *raw)->void *{auto *p=static_cast<Probe *>(raw);p->result=rea_pthread_mutex_trylock(p->foreign);if(p->result==0)rea_pthread_mutex_unlock(p->foreign);return nullptr;};
        int created=pthread_create(&worker,nullptr,tryOther,&probe);
        if(!created)pthread_join(worker,nullptr);
        okay=okay&&created==0&&probe.result==EBUSY;
        rea_pthread_mutex_unlock(second);rea_pthread_mutex_destroy(second);
        okay=okay&&rea_pthread_cond_init(first,nullptr)==0&&rea_pthread_cond_init(second,nullptr)==0;
        LockTable();auto *condition=CondFor(second);UnlockTable();rea_pthread_cond_destroy(first);
        LockTable();okay=okay&&CondFor(second)==condition;UnlockTable();rea_pthread_cond_destroy(second);
        okay=okay&&rea_pthread_rwlock_init(first,nullptr)==0&&rea_pthread_rwlock_init(second,nullptr)==0;
        LockTable();auto *rw=RwlockFor(second);UnlockTable();rea_pthread_rwlock_destroy(first);
        LockTable();okay=okay&&RwlockFor(second)==rw;UnlockTable();rea_pthread_rwlock_destroy(second);
        offset+=snprintf(report+offset,sizeof(report)-offset,"delete collision keeps lock identity/exclusion: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }
    {
        // Signal-safe semaphore lookup must also survive deletion in a collision chain.
        std::vector<uint64_t> slots(kTableSize+1);void *first=&slots[0],*second=&slots[kTableSize];
        bool okay=rea_sem_init(first,0,0)==0&&rea_sem_init(second,0,0)==0;
        LockTable();auto *original=SemFor(second);UnlockTable();rea_sem_destroy(first);
        LockTable();okay=okay&&SemFor(second)==original;UnlockTable();
        okay=okay&&rea_sem_post(second)==0&&rea_sem_trywait(second)==0;rea_sem_destroy(second);
        offset+=snprintf(report+offset,sizeof(report)-offset,"semaphore collision deletion and signal lookup: %s\n",okay?"PASS":"FAIL");okay?passed++:failed++;
    }
    LockTable();g_mutexPeak=g_mutexes.size();g_condPeak=g_conds.size();g_rwPeak=g_rwlocks.size();UnlockTable();
    offset += snprintf(report + offset, sizeof(report) - offset,
                       "ABI_SELFTEST %s passed=%d failed=%d",
                       failed == 0 ? "PASS" : "FAIL", passed, failed);
    g_abiSelfTest = std::string(report);
    return g_abiSelfTest.c_str();
}
