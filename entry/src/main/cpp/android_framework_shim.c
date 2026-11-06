/*
 * libandroid.so replacement for running the Android Unity player on HarmonyOS.
 *
 * HarmonyOS ships no libandroid.so, so the loader resolves that name to this library. It covers
 * every Android-NDK symbol libunity.so imports in that family:
 *
 *   ANativeWindow_*   7   the surface handle Unity renders into
 *   ALooper_*         6   the message pump Unity polls each frame
 *   ASensor_*        15   motion sensors (unused by a rhythm game; stubbed)
 *   AHardwareBuffer_*  3  graphics buffers used by Vulkan/HW paths
 *   __system_property_*  property service
 *
 * Window handling: on HarmonyOS the EGL native window type is OHNativeWindow*, so the host
 * registers that pointer once and ANativeWindow_fromSurface hands it back unchanged. That keeps
 * eglCreateWindowSurface working without a JNI Surface object.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include <hilog/log.h>

#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "ReaAndroidShim", __VA_ARGS__)
#define LOGW(...) OH_LOG_Print(LOG_APP, LOG_WARN, 0x0000, "ReaAndroidShim", __VA_ARGS__)

/* ---- window registration ---------------------------------------------------------------- */

static void *g_native_window = NULL;
static int32_t g_window_width = 1280;
static int32_t g_window_height = 720;
static int32_t g_window_format = 1; /* WINDOW_FORMAT_RGBA_8888 */
static int g_window_refs = 1;

/* Called by the HarmonyOS host once it owns a real OHNativeWindow. */
void ReaShim_RegisterNativeWindow(void *window, int32_t width, int32_t height)
{
    g_native_window = window;
    g_window_width = width > 0 ? width : g_window_width;
    g_window_height = height > 0 ? height : g_window_height;
    LOGI("window registered %{public}dx%{public}d ptr=%{public}p", g_window_width, g_window_height, window);
}

static void *ReaShimLocalWindow(void) { return g_native_window; }

/* ---- ANativeWindow ----------------------------------------------------------------------- */

typedef void ANativeWindow;

/*
 * The HarmonyOS side publishes its window from an ArkUI XComponent, which lives in the probe module
 * rather than here. Resolving it dynamically avoids a link cycle between the two libraries while
 * still preferring the live window over this file's own fallback slot.
 */
static void *PublishedWindow(void)
{
    void *window = NULL;
    void *(*getter)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "ReaShim_GetNativeWindow");
    if (getter) window = getter();
    return window ? window : ReaShimLocalWindow();
}

ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env;
    void *window=NULL;
    int (*surfaceWindow)(void *,void **)=(int (*)(void *,void **))dlsym(RTLD_DEFAULT,"ReaShim_JavaSurfaceWindow");
    if(surfaceWindow&&surfaceWindow(surface,&window))return (ANativeWindow *)window;
    window = PublishedWindow();
    if (!window) {
        LOGW("ANativeWindow_fromSurface without a published OHNativeWindow");
        return NULL;
    }
    g_window_refs++;
    LOGI("ANativeWindow_fromSurface -> %{public}p", window);
    return (ANativeWindow *)window;
}

ANativeWindow *ANativeWindow_fromSurfaceTexture(void *env, void *surfaceTexture)
{
    return ANativeWindow_fromSurface(env, surfaceTexture);
}

void ANativeWindow_acquire(ANativeWindow *window)
{
    (void)window;
    g_window_refs++;
}

void ANativeWindow_release(ANativeWindow *window)
{
    (void)window;
    if (g_window_refs > 0) g_window_refs--;
}

int32_t ANativeWindow_getWidth(ANativeWindow *window)
{
    (void)window;
    int32_t width = 0;
    int32_t height = 0;
    int32_t (*sizeGetter)(int32_t *, int32_t *) =
        (int32_t (*)(int32_t *, int32_t *))dlsym(RTLD_DEFAULT, "ReaShim_GetNativeWindowSize");
    if (sizeGetter && sizeGetter(&width, &height) && width > 0) {
        g_window_width = width;
        return width;
    }
    return g_window_width;
}

int32_t ANativeWindow_getHeight(ANativeWindow *window)
{
    (void)window;
    int32_t width = 0;
    int32_t height = 0;
    int32_t (*sizeGetter)(int32_t *, int32_t *) =
        (int32_t (*)(int32_t *, int32_t *))dlsym(RTLD_DEFAULT, "ReaShim_GetNativeWindowSize");
    if (sizeGetter && sizeGetter(&width, &height) && height > 0) {
        g_window_height = height;
        return height;
    }
    return g_window_height;
}

int32_t ANativeWindow_getFormat(ANativeWindow *window)
{
    (void)window;
    return g_window_format;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format)
{
    (void)window;
    if (width > 0) g_window_width = width;
    if (height > 0) g_window_height = height;
    g_window_format = format;
    LOGI("setBuffersGeometry %{public}dx%{public}d fmt=%{public}d", g_window_width, g_window_height, format);
    return 0;
}

int32_t ANativeWindow_setBuffersTransform(ANativeWindow *window, int32_t transform)
{
    (void)window;
    (void)transform;
    return 0;
}

void *ANativeWindow_toSurface(void *env, ANativeWindow *window)
{
    (void)env;
    (void)window;
    /* No JNI Surface on HarmonyOS; the loop that consumes this handles NULL. */
    return NULL;
}

/* ---- eglGetProcAddress interposition ------------------------------------------------------ */

/*
 * Unity's GLES backend resolves every GL entry point through eglGetProcAddress and calls the result
 * without a null check. HarmonyOS's EGL only reports the functions it considers extensions, so the
 * core GLES ones come back NULL and the engine branches to address 0.
 *
 * This library appears earlier than libEGL.so in libunity's DT_NEEDED order, so exporting the same
 * name here interposes it: fall back to a direct lookup in libGLESv3 before giving up.
 */
typedef void (*ReaEglProc)(void);

ReaEglProc eglGetProcAddress(const char *name)
{
    static void *eglLibrary = NULL;
    static void *glesLibrary = NULL;
    static ReaEglProc (*realGetProcAddress)(const char *) = NULL;
    static int reported = 0;

    if (!eglLibrary) {
        eglLibrary = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
        if (eglLibrary) {
            realGetProcAddress = (ReaEglProc(*)(const char *))dlsym(eglLibrary, "eglGetProcAddress");
        }
    }
    if (!glesLibrary) {
        glesLibrary = dlopen("libGLESv3.so", RTLD_LAZY | RTLD_LOCAL);
    }

    if (!name) return NULL;

    ReaEglProc resolved = NULL;
    if (glesLibrary) resolved = (ReaEglProc)dlsym(glesLibrary, name);
    if (!resolved && realGetProcAddress) resolved = realGetProcAddress(name);
    if (!resolved && eglLibrary) resolved = (ReaEglProc)dlsym(eglLibrary, name);

    static int calls = 0;
    if (calls < 12) {
        calls++;
        LOGI("eglGetProcAddress(\"%{public}s\") -> %{public}p", name, (void *)resolved);
    }
    if (!resolved && reported < 40) {
        reported++;
        LOGW("eglGetProcAddress(\"%{public}s\") -> NULL", name);
    }
    return resolved;
}

typedef struct {
    int read_fd;
    int write_fd;
    int owns;
    int refs;
} ReaLooper;

static __thread ReaLooper *tls_looper = NULL;
static ReaLooper *main_looper = NULL;

static ReaLooper *looper_create(void)
{
    ReaLooper *looper = (ReaLooper *)calloc(1, sizeof(ReaLooper));
    if (!looper) return NULL;
    int fds[2];
    if (pipe(fds) != 0) {
        free(looper);
        return NULL;
    }
    looper->read_fd = fds[0];
    looper->write_fd = fds[1];
    looper->owns = 1;
    looper->refs = 1;
    return looper;
}

void *ALooper_prepare(int32_t opts)
{
    (void)opts;
    if (!tls_looper) {
        tls_looper = looper_create();
        if (tls_looper) LOGI("ALooper_prepare created looper %{public}p", tls_looper);
    }
    return tls_looper;
}

void *ALooper_forThread(void) { return tls_looper; }

void ALooper_acquire(void *looper)
{
    if (looper) ((ReaLooper *)looper)->refs++;
}

void ALooper_release(void *looper)
{
    ReaLooper *l = (ReaLooper *)looper;
    if (!l) return;
    if (--l->refs > 0) return;
    if (l->owns) {
        close(l->read_fd);
        close(l->write_fd);
    }
    if (l == tls_looper) tls_looper = NULL;
    free(l);
}

void ALooper_wake(void *looper)
{
    ReaLooper *l = (ReaLooper *)looper;
    if (!l || !l->owns) return;
    char byte = 1;
    ssize_t ignored = write(l->write_fd, &byte, 1);
    (void)ignored;
}

/*
 * pollOnce with no file-descriptor registration: Unity only uses this to block until the next
 * frame while its own thread drives rendering, so a timed wait plus the wake pipe is enough.
 */
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    if (outFd) *outFd = -1;
    if (outEvents) *outEvents = 0;
    if (outData) *outData = NULL;

    ReaLooper *l = tls_looper;
    if (!l) {
        if (timeoutMillis > 0) usleep((useconds_t)timeoutMillis * 1000);
        return -3; /* ALOOPER_POLL_TIMEOUT */
    }
    struct pollfd pfd;
    pfd.fd = l->read_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, timeoutMillis);
    if (rc == 0) return -3;
    if (rc < 0) return -4; /* ALOOPER_POLL_ERROR */
    char drain[64];
    ssize_t ignored = read(l->read_fd, drain, sizeof(drain));
    (void)ignored;
    return -2; /* ALOOPER_POLL_WAKE */
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    return ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
}

int32_t ALooper_addFd(void *looper, int fd, int ident, int events, void *callback, void *data)
{
    (void)looper; (void)fd; (void)ident; (void)events; (void)callback; (void)data;
    return -1;
}

int32_t ALooper_removeFd(void *looper, int fd)
{
    (void)looper; (void)fd;
    return -1;
}

/* ---- sensors: a rhythm game never needs them -------------------------------------------- */

typedef struct { int type; } ReaSensor;
typedef struct { int unused; } ReaSensorEventQueue;
typedef struct { int unused; } ReaSensorManager;

static ReaSensorManager g_sensor_manager;
static ReaSensorEventQueue g_sensor_queue;

void *ASensorManager_getInstance(void) { return &g_sensor_manager; }
void *ASensorManager_getInstanceForPackage(const char *packageName) { (void)packageName; return &g_sensor_manager; }

int ASensorManager_getSensorList(void *manager, void *list)
{
    (void)manager;
    (void)list;
    return 0;
}

void *ASensorManager_getDefaultSensor(void *manager, int type)
{
    (void)manager;
    (void)type;
    return NULL;
}

void *ASensorManager_createEventQueue(void *manager, void *looper, int ident, void *callback, void *data)
{
    (void)manager; (void)looper; (void)ident; (void)callback; (void)data;
    return &g_sensor_queue;
}

int ASensorManager_destroyEventQueue(void *manager, void *queue)
{
    (void)manager;
    (void)queue;
    return 0;
}

int ASensorEventQueue_enableSensor(void *queue, void *sensor) { (void)queue; (void)sensor; return 0; }
int ASensorEventQueue_disableSensor(void *queue, void *sensor) { (void)queue; (void)sensor; return 0; }
int ASensorEventQueue_setEventRate(void *queue, void *sensor, int32_t usec) { (void)queue; (void)sensor; (void)usec; return 0; }
int ASensorEventQueue_hasEvents(void *queue) { (void)queue; return 0; }
int ASensorEventQueue_getEvents(void *queue, void *events, size_t count) { (void)queue; (void)events; (void)count; return 0; }

const char *ASensor_getName(void *sensor) { (void)sensor; return "ohos-stub"; }
const char *ASensor_getVendor(void *sensor) { (void)sensor; return "ohos"; }
int ASensor_getType(void *sensor) { (void)sensor; return 0; }
float ASensor_getResolution(void *sensor) { (void)sensor; return 0.0f; }
int ASensor_getMinDelay(void *sensor) { (void)sensor; return 0; }
const char *ASensor_getStringType(void *sensor) { (void)sensor; return "stub"; }
int ASensor_getReportingMode(void *sensor) { (void)sensor; return 0; }

/* ---- AHardwareBuffer ------------------------------------------------------------------- */

typedef struct { uint32_t width, height, layers, format, usage; uint32_t stride, rfu0, rfu1; uint64_t rfu2; } AHardwareBuffer_Desc;
typedef struct ReaHardwareBuffer { AHardwareBuffer_Desc desc; int refs; } AHardwareBuffer;

void AHardwareBuffer_acquire(AHardwareBuffer *buffer)
{
    if (buffer) buffer->refs++;
}

void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
    if (!buffer) return;
    if (--buffer->refs <= 0) free(buffer);
}

int AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *outDesc)
{
    if (!outDesc) return -1;
    memset(outDesc, 0, sizeof(*outDesc));
    if (buffer) *outDesc = buffer->desc;
    return 0;
}

void *AHardwareBuffer_fromSurfaceTexture(void *env, void *surfaceTexture)
{
    (void)env; (void)surfaceTexture;
    return NULL;
}

/* ---- system properties ------------------------------------------------------------------ */

/* Opaque prop_info placeholder; libunity only forwards it to __system_property_read. */
typedef struct { const char *name; const char *value; } ReaPropInfo;
static ReaPropInfo g_prop_unknown = { "", "" };

void *__system_property_find(const char *name)
{
    g_prop_unknown.name = name ? name : "";
    g_prop_unknown.value = "";
    return &g_prop_unknown;
}

int __system_property_read(const void *pi, char *name, char *value)
{
    const ReaPropInfo *info = (const ReaPropInfo *)pi;
    if (!info) return 0;
    if (name) strcpy(name, info->name);
    if (value) strcpy(value, info->value);
    return 0;
}

int __system_property_get(const char *name, char *value)
{
    (void)name;
    if (value) value[0] = '\0';
    return 0;
}
