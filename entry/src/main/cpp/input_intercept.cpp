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

extern "C" void ReaQueueTouch(int action, int id, double x, double y, double sourceTimestamp);

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

    ReaQueueTouch(action, id, (double)x, (double)y, (double)(micros * 1000));

    if (++g_events <= 20) {
        LOGI("INTERCEPT_TOUCH action=%{public}d id=%{public}d x=%{public}d y=%{public}d", action, id, x, y);
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
