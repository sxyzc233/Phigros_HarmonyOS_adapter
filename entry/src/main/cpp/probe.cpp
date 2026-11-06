#include "player_platform.h"
/*
 * Translation probe: load the ORIGINAL Android Unity player on HarmonyOS and report exactly how
 * far it gets.
 *
 * Nothing here is game code. It answers one question with device evidence instead of guesswork:
 * with the three shims installed, does the Android libunity.so map, resolve and enter its own
 * initialisation on HarmonyOS?
 *
 * Reports go to hilog under the TransProbe tag and are also returned to ArkTS as a string.
 */
#include <dlfcn.h>
#include <string>
#include <cstdio>
#include <cstring>
#include "tap_sdk_bridge.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hilog/log.h>
#include <napi/native_api.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransProbe"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

std::string g_report;

void Append(const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_report += buffer;
    g_report += "\n";
    LOGI("%{public}s", buffer);
}

/* Load one library and report the loader's own error text when it fails. */
void *Load(const char *name, int flags, const char *why)
{
    dlerror();
    void *handle = dlopen(name, flags);
    if (!handle) {
        const char *error = dlerror();
        Append("LOAD_FAIL %s (%s): %s", name, why, error ? error : "(no dlerror text)");
        return nullptr;
    }
    Append("LOAD_OK   %s (%s) handle=%p", name, why, handle);
    return handle;
}

void ReportSymbol(void *handle, const char *library, const char *symbol)
{
    if (!handle) return;
    dlerror();
    void *address = dlsym(handle, symbol);
    const char *error = dlerror();
    if (address) {
        Append("  SYM %s :: %s = %p", library, symbol, address);
    } else {
        Append("  SYM_MISSING %s :: %s (%s)", library, symbol, error ? error : "?");
    }
}

std::string RunProbe()
{
    g_report.clear();
    Append("=== translation probe start ===");

    /* The Android libraries reference each other, so everything is RTLD_GLOBAL. */
    const int flags = RTLD_NOW | RTLD_GLOBAL;

    /* libil2cpp first: it has no dependency on the engine and proves the shim layer works. */
    void *il2cpp = Load("libil2cpp.so", flags, "il2cpp payload");

    void *main = Load("libmain.so", flags, "unity java bridge");
    void *unity = Load("libunity.so", flags, "unity engine");

    ReportSymbol(main, "libmain.so", "JNI_OnLoad");
    ReportSymbol(unity, "libunity.so", "JNI_OnLoad");

    if (unity) {
        /* A few entry points that only exist once the engine is really there. */
        ReportSymbol(unity, "libunity.so", "unity_init");
        ReportSymbol(unity, "libunity.so", "UnitySendMessage");
        ReportSymbol(unity, "libunity.so", "eglGetProcAddress");
    }
    if (il2cpp) {
        ReportSymbol(il2cpp, "libil2cpp.so", "il2cpp_init");
        ReportSymbol(il2cpp, "libil2cpp.so", "il2cpp_domain_get");
        ReportSymbol(il2cpp, "libil2cpp.so", "il2cpp_set_data_dir");
    }

    Append("RESULT summary il2cpp=%s main=%s unity=%s",
           il2cpp ? "ok" : "fail", main ? "ok" : "fail", unity ? "ok" : "fail");
    Append("=== translation probe end ===");
    return g_report;
}

napi_value Run(napi_env env, napi_callback_info info)
{
    std::string report = RunProbe();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report.c_str(), report.size(), &out);
    return out;
}

/* Implemented in jni_emu.cpp: builds the fake JVM and calls libmain's JNI_OnLoad. */
extern "C" const char *ReaJniProbe();
/* Implemented in jni_emu.cpp: also drives NativeLoader.load so libunity registers its natives. */
extern "C" const char *ReaUnityProbe();
/* Implemented in jni_emu.cpp: finds the installed HAP's rawfile root. */
extern "C" const char *ReaPathProbe();
/* Implemented in jni_emu.cpp: reads the engine's own tombstone from the previous run. */
extern "C" const char *ReaDumpTombstone();
extern "C" const char *ReaIl2CppProbe();
extern "C" const char *ReaRunTracedGfx();
extern "C" const char *ReaBreakpointTrace();
extern "C" const char *ReaAbiSelfTest();
extern "C" void ReaQueueTouch(int,int,double,double,double);
extern "C" bool ReaNativeTouchAvailable();
extern "C" void ReaBindNativeTouch(napi_env,napi_value);
extern "C" void ReaSetSystemLocale(const char *);
extern "C" void ReaSetNetworkType(int);
extern "C" void ReaSetSystemProxy(const char *);
extern "C" void ReaAddSystemTrustRoot(const unsigned char *,size_t);
extern "C" void ReaSetSdkAuthState(int,int,const char *,bool);
napi_value SetSdkAuthState(napi_env env,napi_callback_info info){
    size_t count=4;napi_value args[4];napi_get_cb_info(env,info,&count,args,nullptr,nullptr);
    int age=-1,remaining=0;bool allowed=false;size_t size=0;
    if(count==4&&napi_get_value_int32(env,args[0],&age)==napi_ok&&napi_get_value_int32(env,args[1],&remaining)==napi_ok&&
       napi_get_value_bool(env,args[3],&allowed)==napi_ok&&napi_get_value_string_utf8(env,args[2],nullptr,0,&size)==napi_ok&&size<=32768){
        std::vector<char> token(size+1);napi_get_value_string_utf8(env,args[2],token.data(),token.size(),&size);
        ReaSetSdkAuthState(age,remaining,token.data(),allowed);
    }
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value BridgeRequests(napi_env env,napi_callback_info){
    auto batch=ReaSdkTakeRequests();napi_value array;napi_create_array_with_length(env,batch.size(),&array);
    for(size_t i=0;i<batch.size();++i){napi_value object,id,command;napi_create_object(env,&object);
        napi_create_uint32(env,batch[i].id,&id);napi_create_string_utf8(env,batch[i].command.c_str(),batch[i].command.size(),&command);
        napi_set_named_property(env,object,"id",id);napi_set_named_property(env,object,"command",command);napi_set_element(env,array,i,object);
    }return array;
}
napi_value ReleaseBridge(napi_env env,napi_callback_info info){
    size_t count=1;napi_value arg;napi_get_cb_info(env,info,&count,&arg,nullptr,nullptr);uint32_t id=0;
    if(count==1&&napi_get_value_uint32(env,arg,&id)==napi_ok)ReaSdkRelease(id);
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value PublishCrashReport(napi_env env,napi_callback_info info){
    size_t count=1;napi_value argument;napi_get_cb_info(env,info,&count,&argument,nullptr,nullptr);
    char buffer[512]{};size_t length=0;bool okay=false;
    if(count==1&&napi_get_value_string_utf8(env,argument,buffer,sizeof(buffer),&length)==napi_ok&&length<sizeof(buffer)){
        std::string path(buffer,length);
        const std::string prefix="/data/storage/el2/base/haps/entry/files/rea-crash-diagnostics/report-";
        bool valid=path.rfind(prefix,0)==0&&path.size()>prefix.size()+4&&path.substr(path.size()-4)==".txt";
        for(size_t i=prefix.size();valid&&i<path.size()-4;++i)if(path[i]<'0'||path[i]>'9')valid=false;
        if(valid){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd>=0){struct stat status{};
            okay=fstat(fd,&status)==0&&S_ISREG(status.st_mode)&&status.st_size<=4*1024*1024&&fchmod(fd,0644)==0;close(fd);}}
    }
    napi_value out;napi_get_boolean(env,okay,&out);return out;
}
napi_value ReplyBridge(napi_env env,napi_callback_info info){
    size_t count=3;napi_value args[3];napi_get_cb_info(env,info,&count,args,nullptr,nullptr);
    uint32_t id=0;bool finished=false,accepted=false;size_t size=0;
    if(count==3&&napi_get_value_uint32(env,args[0],&id)==napi_ok&&napi_get_value_bool(env,args[2],&finished)==napi_ok&&
       napi_get_value_string_utf8(env,args[1],nullptr,0,&size)==napi_ok&&size>0&&size<=262144){
        std::vector<char> buffer(size+1);napi_get_value_string_utf8(env,args[1],buffer.data(),buffer.size(),&size);
        accepted=ReaSdkReply(id,std::string(buffer.data(),size),finished);
    }
    napi_value out;napi_get_boolean(env,accepted,&out);return out;
}
napi_value SetSystemProxy(napi_env env,napi_callback_info info){
    size_t count=1;napi_value argument;napi_get_cb_info(env,info,&count,&argument,nullptr,nullptr);
    char text[4096]{};size_t length=0;
    if(count==1&&napi_get_value_string_utf8(env,argument,text,sizeof(text),&length)==napi_ok)ReaSetSystemProxy(text);
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value AddSystemTrustRoot(napi_env env,napi_callback_info info){
    size_t count=1;napi_value argument;napi_get_cb_info(env,info,&count,&argument,nullptr,nullptr);
    napi_typedarray_type type;size_t length=0,offset=0;void *data=nullptr;napi_value arraybuffer;
    if(count==1&&napi_get_typedarray_info(env,argument,&type,&length,&data,&arraybuffer,&offset)==napi_ok&&type==napi_uint8_array)
        ReaAddSystemTrustRoot(static_cast<unsigned char *>(data),length);
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value SetNetworkType(napi_env env,napi_callback_info info){
    size_t count=1;napi_value argument;napi_get_cb_info(env,info,&count,&argument,nullptr,nullptr);int type=0;
    if(count==1&&napi_get_value_int32(env,argument,&type)==napi_ok)ReaSetNetworkType(type);
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value SetSystemLocale(napi_env env,napi_callback_info info){
    size_t count=1;napi_value argument;napi_get_cb_info(env,info,&count,&argument,nullptr,nullptr);
    char text[96]{};size_t length=0;
    if(count==1&&napi_get_value_string_utf8(env,argument,text,sizeof(text),&length)==napi_ok)ReaSetSystemLocale(text);
    napi_value out;napi_get_undefined(env,&out);return out;
}
napi_value NativeTouchAvailable(napi_env env,napi_callback_info)
{
    napi_value out;napi_get_boolean(env,ReaNativeTouchAvailable(),&out);return out;
}

napi_value Touch(napi_env env,napi_callback_info info)
{
    size_t count=5;napi_value args[5];
    napi_get_cb_info(env,info,&count,args,nullptr,nullptr);
    int action=0,id=0;double x=0,y=0,timestamp=0;
    if(count==5 && napi_get_value_int32(env,args[0],&action)==napi_ok &&
        napi_get_value_int32(env,args[1],&id)==napi_ok && napi_get_value_double(env,args[2],&x)==napi_ok &&
        napi_get_value_double(env,args[3],&y)==napi_ok && napi_get_value_double(env,args[4],&timestamp)==napi_ok)ReaQueueTouch(action,id,x,y,timestamp);
    napi_value result;napi_get_undefined(env,&result);return result;
}

napi_value RunTombstone(napi_env env, napi_callback_info info)
{
    const char *report = ReaDumpTombstone();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunPaths(napi_env env, napi_callback_info info)
{
    const char *report = ReaPathProbe();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunJni(napi_env env, napi_callback_info info)
{
    const char *report = ReaJniProbe();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunUnity(napi_env env, napi_callback_info info)
{
    const char *report = ReaUnityProbe();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

/* Implemented in surface.cpp: turns an ArkUI surface id into an OHNativeWindow for EGL. */
extern "C" int32_t ReaSetSurfaceId(const char *surfaceIdText);

napi_value SetSurface(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char text[64] = {0};
    size_t length = 0;
    if (argc > 0 && napi_get_value_string_utf8(env, argv[0], text, sizeof(text), &length) == napi_ok) {
        ReaSetSurfaceId(text);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value RunIl2Cpp(napi_env env, napi_callback_info info)
{
    const char *report = ReaIl2CppProbe();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

/* Implemented in jni_emu.cpp: deliberate, instrumented crash for layout calibration. */
extern "C" void ReaCalibrateCrash(void);

napi_value RunCalibrate(napi_env env, napi_callback_info info)
{
    ReaCalibrateCrash();
    napi_value out = nullptr;
    napi_get_boolean(env, false, &out);
    return out;
}

napi_value RunTraced(napi_env env, napi_callback_info info)
{
    const char *report = ReaRunTracedGfx();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunBreakpoint(napi_env env, napi_callback_info info)
{
    const char *report = ReaBreakpointTrace();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value RunAbiTest(napi_env env, napi_callback_info info)
{
    const char *report = ReaAbiSelfTest();
    napi_value out = nullptr;
    napi_create_string_utf8(env, report, NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor descriptors[] = {
        {"run", nullptr, Run, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jni", nullptr, RunJni, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unity", nullptr, RunUnity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"paths", nullptr, RunPaths, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSurface", nullptr, SetSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"tombstone", nullptr, RunTombstone, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"il2cpp", nullptr, RunIl2Cpp, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"calibrate", nullptr, RunCalibrate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"traced", nullptr, RunTraced, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"breakpoint", nullptr, RunBreakpoint, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"abitest", nullptr, RunAbiTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"touch", nullptr, Touch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTouchAvailable", nullptr, NativeTouchAvailable, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSystemLocale", nullptr, SetSystemLocale, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setNetworkType", nullptr, SetNetworkType, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSystemProxy", nullptr, SetSystemProxy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addSystemTrustRoot", nullptr, AddSystemTrustRoot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"bridgeRequests", nullptr, BridgeRequests, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"replyBridge", nullptr, ReplyBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSdkAuthState", nullptr, SetSdkAuthState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseBridge", nullptr, ReleaseBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"publishCrashReport", nullptr, PublishCrashReport, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(descriptors)/sizeof(descriptors[0]), descriptors);
    ReaExportPlatform(env,exports);
    ReaBindNativeTouch(env,exports);
    return exports;
}

} // namespace

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "transprobe",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterTransProbe(void)
{
    napi_module_register(&g_module);
}
