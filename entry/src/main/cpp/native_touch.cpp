#include "player_platform.h"
#include "host_lifecycle.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <napi/native_api.h>
#include <hilog/log.h>
#include <atomic>
#include <cstdint>
#include <time.h>

extern "C" void ReaQueueTouch(int,int,double,double,double);
extern "C" void ReaQueueTouchSnapshot(int,int,double,double,const int*,const double*,const double*,int,double);
extern "C" void ReaUseNativeSurface(void *);
static std::atomic<bool> g_nativeTouch{false};

/*
 * Refresh the XComponent asks the compositor for, in Hz. The panel's own ceiling still wins: if the
 * display tops out lower, the request is clamped and the frame pacer keeps following the real callback
 * cadence. 144 is what this panel can do and it is the single biggest lever on touch-to-photon latency -
 * a 120 Hz request leaves ~8 ms of frame time in the path, 144 Hz leaves ~7 ms, and the measured
 * input-to-render p50 sat at 15.8 ms even after exclusive touch. The options sheet can lower it.
 */
static std::atomic<int32_t> g_expectedHz{144};
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
/* Frame counters for the diagnostic line below; cheap enough to keep in release. */
static uint64_t g_frames=0,g_histDown=0,g_histUp=0,g_popped=0,g_lastLogFrames=0;
static int64_t g_lastLogNs=0;
static int64_t FrameClockNs(){timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (int64_t)now.tv_sec*1000000000LL+now.tv_nsec;}

static void DispatchTouch(OH_NativeXComponent *component,void *window)
{
    OH_NativeXComponent_TouchEvent event{};
    if(OH_NativeXComponent_GetTouchEvent(component,window,&event)!=OH_NATIVEXCOMPONENT_RESULT_SUCCESS)return;
    // The native API supplies component-local pixel coordinates and monotonic
    // nanoseconds. It avoids ArkTS dispatch, vp conversion and per-frame batching.
    if(event.type<OH_NATIVEXCOMPONENT_DOWN || event.type>OH_NATIVEXCOMPONENT_CANCEL)return;

    /*
     * This callback runs once per frame and carries the whole pressed set in touchPoints/numPoints -
     * the frame's other touch events only survive as historical points. Forwarding event.id alone (the
     * changed pointer) drops every other finger's Down, which is what made multi-touch break: the
     * engine never learned those ids were down and discarded their later moves and lifts.
     */
    uint32_t total=event.numPoints;
    if(total>OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER)total=OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER;
    // isPressed is only trusted when the platform actually fills it; some builds leave it false while
    // still reporting the point, and treating that as "every finger up" would lift everything each frame.
    bool anyPressed=false;
    for(uint32_t i=0;i<total;++i){if(event.touchPoints[i].isPressed){anyPressed=true;break;}}
    int ids[OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER];
    double xs[OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER],ys[OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER];
    int count=0,popped=0;
    for(uint32_t i=0;i<total;++i){
        const OH_NativeXComponent_TouchPoint &point=event.touchPoints[i];
        if(anyPressed && !point.isPressed){++popped;continue;}
        if(point.id<0)continue;
        ids[count]=point.id;xs[count]=point.x;ys[count]=point.y;++count;
    }
    ++g_frames;g_popped+=popped;

    /*
     * The frame set is only the state at the END of the frame, so a finger that went down and came back
     * up inside the same frame never appears in it - the tap is simply invisible to the engine. Those
     * presses/lifts survive as historical points, which the platform documents as "all touch points
     * collected during the current frame". Replaying their Down/Up, in the order they happened, is what
     * restores such a tap: Down then Up becomes a real MotionEvent pair instead of nothing at all.
     *
     * Only Down/Up are replayed. Moves would be stale by the time the frame ends and the reconciliation
     * below already positions every held pointer from the frame-end set.
     */
    int down=0,up=0;
    if(event.type!=OH_NATIVEXCOMPONENT_CANCEL){
        int32_t historyCount=0;
        OH_NativeXComponent_HistoricalPoint *history=nullptr;
        if(OH_NativeXComponent_GetHistoricalPoints(component,window,&historyCount,&history)==
           OH_NATIVEXCOMPONENT_RESULT_SUCCESS && history && historyCount>0){
            if(historyCount>64)historyCount=64;
            /*
             * Guard against a stale entry: if history ever reaches back further than this frame, a Down
             * whose finger is long gone would be injected as a press nothing ever lifts. The frame stamp
             * is the last event of this frame, so anything within one frame width of it is genuinely
             * this frame's input (the panel reports at 120 Hz and up to 1 kHz).
             */
            const int64_t frameStamp=(int64_t)event.timeStamp;
            for(int32_t i=0;i<historyCount;++i){
                const OH_NativeXComponent_HistoricalPoint &point=history[i];
                if(point.id<0)continue;
                if(frameStamp>0 && point.timeStamp>0 && frameStamp-point.timeStamp>50000000LL)continue;
                if(point.type==OH_NATIVEXCOMPONENT_DOWN){ReaQueueTouch(0,point.id,point.x,point.y,(double)point.timeStamp);++down;}
                else if(point.type==OH_NATIVEXCOMPONENT_UP){ReaQueueTouch(1,point.id,point.x,point.y,(double)point.timeStamp);++up;}
            }
        }
        g_histDown+=(uint64_t)down;g_histUp+=(uint64_t)up;
    }

    ReaQueueTouchSnapshot((int)event.type,event.id,event.x,event.y,ids,xs,ys,count,(double)event.timeStamp);

    const int64_t now=FrameClockNs();
    if(g_frames-g_lastLogFrames>=120 || now-g_lastLogNs>=2000000000LL){
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransTouch",
            "TOUCH_FRAME_WINDOW frames=%{public}llu histDown=%{public}llu histUp=%{public}llu popped=%{public}llu points=%{public}d",
            (unsigned long long)g_frames,(unsigned long long)g_histDown,(unsigned long long)g_histUp,
            (unsigned long long)g_popped,count);
        g_lastLogFrames=g_frames;g_lastLogNs=now;
    }
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
