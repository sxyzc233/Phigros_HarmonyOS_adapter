/* Real ArkUI window publication and ability lifecycle. No engine call runs
 * under this lock or on the ArkUI callback thread. */
#include "host_lifecycle.h"
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <hilog/log.h>
#include <native_window/external_window.h>

namespace {
std::mutex stateMutex, windowRefMutex;
std::condition_variable changed;
ReaLifecycleSnapshot state;
}
ReaNativeSurface::ReaNativeSurface(void *value,int32_t w,int32_t h):window(value),width(w),height(h) {
    std::lock_guard<std::mutex> lock(windowRefMutex);
    OH_NativeWindow_NativeObjectReference(window);
}
ReaNativeSurface::~ReaNativeSurface() {
    std::lock_guard<std::mutex> lock(windowRefMutex);
    OH_NativeWindow_NativeObjectUnreference(window);
}
ReaLifecycleSnapshot ReaReadLifecycle() {
    std::lock_guard<std::mutex> lock(stateMutex);return state;
}
void ReaWaitLifecycle(uint64_t serial,int timeoutMs) {
    std::unique_lock<std::mutex> lock(stateMutex);
    changed.wait_for(lock,std::chrono::milliseconds(timeoutMs),[&]{return state.serial!=serial;});
}
void ReaSetAppForeground(bool foreground) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(state.foreground==foreground)return;
        state.foreground=foreground;++state.serial;
        // Preserve the edge when background and foreground arrive between frames.
        if(!foreground)++state.pauseEpoch;
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransLifecycle","ABILITY foreground=%{public}d epoch=%{public}llu",foreground,(unsigned long long)state.pauseEpoch);
    }
    changed.notify_all();
}
extern "C" bool ReaNativeSurfaceAvailable() {return bool(ReaReadLifecycle().surface);}
extern "C" void *ReaShim_GetNativeWindow() {
    auto snapshot=ReaReadLifecycle();return snapshot.surface?snapshot.surface->window:nullptr;
}
extern "C" int32_t ReaShim_GetNativeWindowSize(int32_t *width,int32_t *height) {
    auto snapshot=ReaReadLifecycle();
    if(width)*width=snapshot.surface?snapshot.surface->width:1920;
    if(height)*height=snapshot.surface?snapshot.surface->height:1080;
    return snapshot.surface?1:0;
}
extern "C" void ReaGetTouchBufferScale(float *x,float *y) {
    *x=*y=1;auto snapshot=ReaReadLifecycle();if(!snapshot.surface)return;
    auto &surface=*snapshot.surface;int32_t width=0,height=0;
    if(OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(surface.window),GET_BUFFER_GEOMETRY,&height,&width)==0&&width>0&&height>0) {
        *x=float(width)/surface.width;*y=float(height)/surface.height;
    }
}
extern "C" int ReaGetRenderWindowSize(int32_t *width,int32_t *height) {
    auto snapshot=ReaReadLifecycle();
    *width=snapshot.surface?snapshot.surface->width:1920;*height=snapshot.surface?snapshot.surface->height:1080;
    if(!snapshot.surface)return 0;
    return OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(snapshot.surface->window),GET_BUFFER_GEOMETRY,height,width)==0;
}
extern "C" void ReaUseNativeSurface(void *window) {
    if(!window)return;int32_t width=0,height=0;
    if(OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(window),GET_BUFFER_GEOMETRY,&height,&width)!=0||width<=0||height<=0)return;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(state.surface&&state.surface->window==window&&state.surface->width==width&&state.surface->height==height)return;
        bool replacement=!state.surface||state.surface->window!=window;
        state.surface=std::make_shared<ReaNativeSurface>(window,width,height);
        if(replacement)++state.surfaceEpoch;
        ++state.serial;
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransLifecycle","SURFACE publish=%{public}p size=%{public}dx%{public}d epoch=%{public}llu",window,width,height,(unsigned long long)state.surfaceEpoch);
    }
    changed.notify_all();
}
extern "C" void ReaClearNativeSurface(void *expectedWindow) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if(!state.surface||(expectedWindow&&state.surface->window!=expectedWindow))return;
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransLifecycle","SURFACE withdraw=%{public}p",state.surface->window);
        state.surface.reset();++state.serial;++state.pauseEpoch;
    }
    changed.notify_all();
}
extern "C" int32_t ReaSetSurfaceId(const char *text) {
    if(!text)return 0;uint64_t id=strtoull(text,nullptr,10);if(!id)return 0;
    OHNativeWindow *window=nullptr;
    if(OH_NativeWindow_CreateNativeWindowFromSurfaceId(id,&window)!=0||!window)return 0;
    ReaUseNativeSurface(window);
    {std::lock_guard<std::mutex> lock(windowRefMutex);OH_NativeWindow_DestroyNativeWindow(window);}
    return ReaNativeSurfaceAvailable()?1:0;
}
extern "C" void ReaClearSurfaceId(const char *text) {
    if(!text)return;uint64_t id=strtoull(text,nullptr,10);if(!id)return;
    auto snapshot=ReaReadLifecycle();if(!snapshot.surface)return;
    uint64_t currentId=0;
    if(OH_NativeWindow_GetSurfaceId(static_cast<OHNativeWindow *>(snapshot.surface->window),&currentId)==0&&currentId==id)
        ReaClearNativeSurface(snapshot.surface->window);
}
