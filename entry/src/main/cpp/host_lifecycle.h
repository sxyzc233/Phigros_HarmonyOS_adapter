#pragma once
#include <cstdint>
#include <memory>

// Keep a real window reference until the Unity owner has detached its renderer.
struct ReaNativeSurface {
    void *window;
    int32_t width, height;
    ReaNativeSurface(void *value, int32_t w, int32_t h);
    ~ReaNativeSurface();
    ReaNativeSurface(const ReaNativeSurface &) = delete;
    ReaNativeSurface &operator=(const ReaNativeSurface &) = delete;
};
struct ReaLifecycleSnapshot {
    bool foreground = false;
    uint64_t serial = 0, pauseEpoch = 0, surfaceEpoch = 0;
    std::shared_ptr<ReaNativeSurface> surface;
};
ReaLifecycleSnapshot ReaReadLifecycle();
void ReaSetAppForeground(bool foreground);
void ReaWaitLifecycle(uint64_t serial, int timeoutMs);
extern "C" void ReaClearNativeSurface(void *expectedWindow);
extern "C" void ReaClearSurfaceId(const char *text);
extern "C" bool ReaNativeSurfaceAvailable();
