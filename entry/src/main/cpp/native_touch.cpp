#include "player_platform.h"
#include "host_lifecycle.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <napi/native_api.h>
#include <hilog/log.h>
#include <atomic>

extern "C" void ReaQueueTouch(int,int,double,double,double);
extern "C" void ReaUseNativeSurface(void *);
static std::atomic<bool> g_nativeTouch{false};

/*
 * Refresh the XComponent asks the compositor for, in Hz. The panel's own ceiling still wins: if the
 * display tops out lower, the request is clamped and the frame pacer keeps following the real callback
 * cadence. 120 is what the game shipped with; the options sheet can raise it at runtime.
 */
static std::atomic<int32_t> g_expectedHz{120};
static OH_NativeXComponent *g_component = nullptr;

static void DisplayFrame(OH_NativeXComponent *,uint64_t timestamp,uint64_t target){ReaDisplayFrame(timestamp,target);}
static void SurfaceCreated(OH_NativeXComponent *component,void *window){
    g_nativeTouch=true;ReaUseNativeSurface(window);
    const int32_t hz=g_expectedHz.load();
    OH_NativeXComponent_ExpectedRateRange range{30,hz,hz};
    int rate=OH_NativeXComponent_SetExpectedFrameRateRange(component,&range);
    int callback=OH_NativeXComponent_RegisterOnFrameCallback(component,DisplayFrame);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransFrame","DISPLAY_RATE_REQUEST expected=%{public}d result=%{public}d callback=%{public}d",hz,rate,callback);
}

/*
 * Runtime request from the options sheet. Returns the platform result code: 0 means the compositor
 * accepted the range, anything else means it refused and the panel keeps its own rate. A request made
 * while no surface exists is remembered and applied by the next SurfaceCreated.
 */
extern "C" int ReaRequestFrameRate(int hz)
{
    if(hz<24||hz>240)return -1;
    g_expectedHz.store(hz);
    if(!g_nativeTouch.load()||!g_component){
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransFrame","DISPLAY_RATE_REQUEST expected=%{public}d deferred=1",hz);
        return -2;
    }
    OH_NativeXComponent_ExpectedRateRange range{30,hz,hz};
    int result=OH_NativeXComponent_SetExpectedFrameRateRange(g_component,&range);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransFrame","DISPLAY_RATE_REQUEST expected=%{public}d result=%{public}d live=1",hz,result);
    return result;
}
static void SurfaceChanged(OH_NativeXComponent *,void *window){ReaUseNativeSurface(window);}
static void SurfaceDestroyed(OH_NativeXComponent *component,void *window)
{
    OH_NativeXComponent_UnregisterOnFrameCallback(component);
    g_nativeTouch=false;
    ReaQueueTouch(3,0,0,0,0);
    ReaClearNativeSurface(window);
}
static void DispatchTouch(OH_NativeXComponent *component,void *window)
{
    OH_NativeXComponent_TouchEvent event{};
    if(OH_NativeXComponent_GetTouchEvent(component,window,&event)!=OH_NATIVEXCOMPONENT_RESULT_SUCCESS)return;
    // The native API supplies component-local pixel coordinates and monotonic
    // nanoseconds. It avoids ArkTS dispatch, vp conversion and per-frame batching.
    if(event.type>=OH_NATIVEXCOMPONENT_DOWN && event.type<=OH_NATIVEXCOMPONENT_CANCEL)
        ReaQueueTouch((int)event.type,event.id,event.x,event.y,(double)event.timeStamp);
}
static OH_NativeXComponent_Callback g_callbacks{SurfaceCreated,SurfaceChanged,SurfaceDestroyed,DispatchTouch};

extern "C" bool ReaNativeTouchAvailable(){return g_nativeTouch.load();}
extern "C" void ReaBindNativeTouch(napi_env env,napi_value exports)
{
    bool has=false;
    if(napi_has_named_property(env,exports,OH_NATIVE_XCOMPONENT_OBJ,&has)!=napi_ok || !has)return;
    napi_value value;OH_NativeXComponent *component=nullptr;
    if(napi_get_named_property(env,exports,OH_NATIVE_XCOMPONENT_OBJ,&value)!=napi_ok ||
        napi_unwrap(env,value,(void **)&component)!=napi_ok || !component)return;
    int result=OH_NativeXComponent_RegisterCallback(component,&g_callbacks);
    g_nativeTouch=result==OH_NATIVEXCOMPONENT_RESULT_SUCCESS;
    g_component=component;
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransTouch","NATIVE_TOUCH_BIND result=%{public}d available=%{public}d",result,g_nativeTouch.load()?1:0);
}
