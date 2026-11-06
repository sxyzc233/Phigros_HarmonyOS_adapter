#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <napi/native_api.h>

struct ReaKeyboardState {
    uint32_t revision=0, session=0;
    bool visible=false, secure=false, multiline=false;
    int type=0, limit=0, start=0, length=0;
    std::string text, hint;
};
struct ReaKeyboardEvent {
    uint32_t session=0;
    int status=0, start=0, length=0; // 0 edit, 1 submit, 2 cancel, 3 visibility
    std::string text;
};
void ReaShowKeyboard(const ReaKeyboardState &state);
void ReaChangeKeyboard(const char *text, int limit=-1, int start=-1, int length=0);
void ReaHideKeyboard();
std::vector<ReaKeyboardEvent> ReaTakeKeyboardEvents();
void ReaExportPlatform(napi_env env,napi_value exports);
void ReaRequestPlayerExit(const char *reason);
struct ReaDialogState {
    uint32_t revision=0,session=0;
    int kind=0;
    bool visible=false,cancelable=false;
    std::string title,message;
    std::vector<std::string> buttons;
};
struct ReaDialogResult {std::string target,method,argument;};
void ReaShowNativeDialog(const ReaDialogState &state);
void ReaDismissNativeDialog();
std::vector<ReaDialogResult> ReaTakeDialogResults();
extern "C" void ReaDisplayFrame(uint64_t timestamp,uint64_t targetTimestamp);
extern "C" double ReaDisplayRefreshRate();
extern "C" int64_t ReaNextFrameDelayNs(int64_t minimumDelayNs);
