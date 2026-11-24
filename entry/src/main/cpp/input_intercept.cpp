/*
 * Exclusive touch through ohos.permission.INTERCEPT_INPUT_EVENT.
 *
 * While the interceptor is installed the system hands raw touch events to this callback instead of
 * dispatching them into the window, so ArkUI's onTouch and the XComponent's native callback stop
 * seeing input entirely. Every event is copied out of the callback (the object is only valid inside
 * it) and pushed into the same queue the XComponent path uses, with the original event time, so the
 * engine keeps receiving real input instead of losing it.
 *
 * Only the touch callback is registered; mouse and axis events are not used by the game.
 */
#include <multimodalinput/oh_input_manager.h>
#include <hilog/log.h>

#include <cstdint>
#include <set>

extern "C" void ReaQueueTouch(int action, int id, double x, double y, double sourceTimestamp);
/* Live option lookup from jni_emu.cpp; the tool filter can be switched off from the options sheet. */
extern "C" bool ReaTouchOption(const char *name);

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransInput"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

/*
 * The interceptor reports the platform enum (0 cancel, 1 down, 2 move, 3 up); the engine queue
 * expects Android's MotionEvent order (0 down, 1 up, 2 move, 3 cancel).
 */
int ToQueueAction(int32_t action)
{
    switch (action) {
        case 1: return 0;   /* down   */
        case 3: return 1;   /* up     */
        case 2: return 2;   /* move   */
        case 0: return 3;   /* cancel */
        default: return -1;
    }
}

/*
 * Must outlive the interception: the system keeps this pointer after the registering call returns.
 * Putting it on the stack is what makes the first tap crash.
 */
Input_InterceptorEventCallback g_callbacks{};

bool g_installed = false;
int g_result = -1;
uint64_t g_events = 0;
/* Tool-type census. A knuckle or a palm arrives as its own tool type on some devices; a stylus class
 * tool never belongs to a rhythm-game finger, so those are counted and kept out of the pointer set. */
uint64_t g_toolFinger = 0;
uint64_t g_toolOther = 0;
int g_toolLogs = 0;
/* Press/lift tracing: capped, so a fast multi-finger landing is fully visible in the log. */
int g_pressLogs = 0;
int64_t g_lastDownMicros = 0;

void OnTouch(const Input_TouchEvent *event)
{
    if (event == nullptr) return;

    const int action = ToQueueAction(OH_Input_GetTouchEventAction(event));
    if (action < 0) return;

    const int id = (int)OH_Input_GetTouchEventFingerId(event);
    const int32_t x = OH_Input_GetTouchEventWindowX(event);
    const int32_t y = OH_Input_GetTouchEventWindowY(event);
    /* Action time arrives in microseconds; the queue and the engine both work in nanoseconds. */
    const int64_t micros = OH_Input_GetTouchEventActionTime(event);
    const int tool = (int)OH_Input_GetTouchEventToolType(event);

    if (tool == (int)TOOL_TYPE_FINGER || !ReaTouchOption("toolFilter")) {
        if (tool == (int)TOOL_TYPE_FINGER) ++g_toolFinger;
    } else {
        /*
         * Not a finger. Dropping these is safe for the engine's pointer set: without their Down the
         * later Move/Up are ignored, so a stray pen or palm can never leave a pointer held down.
         */
        ++g_toolOther;
        if (++g_toolLogs <= 8) {
            LOGI("INTERCEPT_TOOL_DROP tool=%{public}d action=%{public}d id=%{public}d x=%{public}d y=%{public}d",
                 tool, action, id, x, y);
        }
        return;
    }

    ReaQueueTouch(action, id, (double)x, (double)y, (double)(micros * 1000));

    if (++g_events <= 20) {
        LOGI("INTERCEPT_TOUCH action=%{public}d id=%{public}d x=%{public}d y=%{public}d", action, id, x, y);
    }
    /*
     * Down/lift detail. `gap` is the distance to the previous down in microseconds: a chord where several
     * fingers land nearly together is exactly what this trace is for, and it also reveals whether the
     * platform hands out the same finger id twice inside that window.
     */
    /*
     * Chord tracing only. A budget of a few hundred lines spent on ordinary notes explains nothing; the
     * case under investigation is several fingers landing inside the same few milliseconds, so only
     * presses that follow the previous press closely are recorded - together with the lift of any finger
     * that arrived in such a burst, which shows whether the engine ever saw it let go.
     */
    static std::set<int> g_chordIds;
    if ((action == 0 || action == 1) && g_pressLogs < 300) {
        const int64_t gap = (action == 0 && g_lastDownMicros > 0) ? micros - g_lastDownMicros : -1;
        if (action == 0) g_lastDownMicros = micros;
        const bool chord = (action == 0 && gap >= 0 && gap < 20000) || (action == 1 && g_chordIds.count(id));
        if (chord) {
            if (action == 0) g_chordIds.insert(id);
            if (action == 1) g_chordIds.erase(id);
            const int32_t pressure = (int32_t)(OH_Input_GetTouchEventPressure(event) * 1000);
            LOGI("INTERCEPT_PRESS action=%{public}d id=%{public}d x=%{public}d y=%{public}d gap_us=%{public}lld pressure=%{public}d",
                 action, id, x, y, (long long)gap, pressure);
            ++g_pressLogs;
        }
    }
    if (g_events % 3000 == 0) {
        LOGI("INTERCEPT_TOOLS finger=%{public}llu other=%{public}llu",
             (unsigned long long)g_toolFinger, (unsigned long long)g_toolOther);
    }
}

} // namespace

/* Returns true when the interceptor is installed; the raw Input_Result goes back for the UI. */
extern "C" bool ReaInputInterceptStart(int *outResult)
{
    if (g_installed) {
        if (outResult) *outResult = g_result;
        return true;
    }

    g_callbacks.mouseCallback = nullptr;
    g_callbacks.touchCallback = OnTouch;
    g_callbacks.axisCallback = nullptr;
    g_events = 0;

    const Input_Result result = OH_Input_AddInputEventInterceptor(&g_callbacks, nullptr);
    g_result = (int)result;
    g_installed = result == INPUT_SUCCESS;
    LOGI("OH_Input_AddInputEventInterceptor result=%{public}d", g_result);
    /*
     * From this moment the window stops receiving touches entirely, so any pointer the XComponent path
     * still holds from the last frame would stay pressed for good - a phantom that makes the final real
     * lift arrive as POINTER_UP instead of UP. Clear the engine's set and start the interceptor clean.
     */
    if (g_installed) ReaQueueTouch(3, 0, 0, 0, 0);
    if (outResult) *outResult = g_result;
    return g_installed;
}

extern "C" bool ReaInputInterceptStop()
{
    if (!g_installed) return true;
    const Input_Result result = OH_Input_RemoveInputEventInterceptor();
    LOGI("OH_Input_RemoveInputEventInterceptor result=%{public}d", (int)result);
    g_installed = false;
    /* Release whatever the game still believes is held down. */
    ReaQueueTouch(3, 0, 0, 0, 0);
    return result == INPUT_SUCCESS;
}

extern "C" void ReaInputInterceptState(int *active, int *lastResult, unsigned long long *events)
{
    if (active) *active = g_installed ? 1 : 0;
    if (lastResult) *lastResult = g_result;
    if (events) *events = (unsigned long long)g_events;
}
