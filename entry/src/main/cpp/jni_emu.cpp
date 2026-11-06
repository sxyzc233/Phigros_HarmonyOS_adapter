/*
 * A miniature Android framework for hosting Unity's Java-facing native code on HarmonyOS.
 *
 * Why this exists: Unity's Android player keeps its Java layer separate from the engine. The engine
 * (libunity.so) does not link any asset API at all - measured: zero AAsset* imports - so it reaches
 * game data exclusively through JNI:
 *
 *     Context.getAssets() -> AssetManager.openFd(path) -> AssetFileDescriptor
 *     -> FileDescriptor.descriptor (an int fd) -> native pread(fd, startOffset + pos, length)
 *
 * Therefore this file supplies just enough of android.content.Context, AssetManager,
 * AssetFileDescriptor, Resources, java.io.File/FileDescriptor, plus the JNI surface to drive them.
 * Everything unhandled is logged with its class/method/signature so the next missing piece is
 * discovered from the device log instead of guessed.
 */
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include "jni_args.hpp"
#include "tap_sdk_bridge.h"
#include "java_preferences.h"
#include "player_platform.h"
#include "video_surface.h"
#include "host_lifecycle.h"
extern "C" void ReaLogEglState(const char *);
extern "C" void ReaDumpThreadAbi();
extern "C" void ReaGetTouchBufferScale(float *,float *);
extern "C" void ReaFmodStart(uintptr_t,void *);
extern "C" void ReaFmodStop();
extern "C" bool ReaFmodRunning();
extern "C" void *ReaFrameworkEnv();
extern "C" int ReaPatchPthreadGOT(void *);
extern "C" void ReaLogEntryState();
extern "C" void ReaConfigureExceptionTrace(void *);
extern "C" void ReaLogUnityJobs();
#include <ucontext.h>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cctype>
#include <unistd.h>
#include <sys/syscall.h>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <codecvt>
#include <locale>
#include <cmath>

#include <dlfcn.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <asm/ptrace.h>
#include <elf.h>
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#include <execinfo.h>
#include <sys/stat.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransJni"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

std::string g_log;
std::mutex g_logMutex;
std::mutex g_objectMutex;
std::mutex g_videoJniMutex;

void Emit(const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    { std::lock_guard<std::mutex> lock(g_logMutex);
      // The bootstrap report is bounded; runtime diagnostics stay in hilog.
      if(g_log.size()<262144){g_log += buffer;g_log += "\n";} }
    LOGI("%{public}s", buffer);
}

/* ---- object model ------------------------------------------------------------------------ */

struct MethodRef {
    std::string cls;
    std::string name;
    std::string signature;
    bool isStatic = false;
    bool isField = false;
};

struct JavaLoop;
struct TouchPoint { int id; float x,y; };
struct FakeObject {
    std::string cls;
    std::map<std::string, int64_t> ints;
    std::map<std::string, void *> refs;
    std::map<std::string, std::string> strs;
    std::u16string utf16;
    std::map<std::string,double> numbers;
    std::vector<TouchPoint> touchPoints;
    std::shared_ptr<ReaNativeSurface> nativeSurface;
    int fd = -1;
    int64_t startOffset = 0;
    int64_t length = 0;
    bool ownsFd = false;
    std::shared_ptr<JavaLoop> loop;
    std::shared_ptr<rea_prefs::Store> preferences;
    std::shared_ptr<rea_prefs::Editor> preferenceEditor;
    std::vector<void *> items;
};

struct JavaTask {
    FakeObject *callback;
    std::string method;
    FakeObject *argument;
    std::chrono::steady_clock::time_point due;
};
struct JavaLoop {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<JavaTask> tasks;
    bool quit = false;
};
thread_local FakeObject *g_threadLooper = nullptr;
static FakeObject *PrepareJavaLoop();
static FakeObject *GetMainJavaLoop();
static void RunJavaLoop(FakeObject *looper);
static void QueueJavaTask(FakeObject *looper, FakeObject *callback, const char *method,
                          FakeObject *argument = nullptr, int64_t delayMs = 0);
static void *InvokeJavaProxy(FakeObject *proxy, const char *method, FakeObject *argument);
static void *JavaThreadBody(void *raw);
static void QueueUnityJob(FakeObject *job,bool inlineOnOwner=false);

std::vector<FakeObject *> g_objects;
std::unordered_set<FakeObject *> g_objectIndex;
std::vector<MethodRef *> g_methods;
std::unordered_multimap<uint64_t,MethodRef *> g_methodIndex;
uint64_t g_objectChecks=0;
std::atomic<uint64_t> g_stringConstructions{0};

bool IsFakeObject(void *candidate)
{
    if (!candidate) return false;
    std::lock_guard<std::mutex> lock(g_objectMutex);
    ++g_objectChecks;
    return g_objectIndex.count(static_cast<FakeObject *>(candidate))!=0;
}

/*
 * JNI's variadic entry points (CallObjectMethodV and friends) receive a va_list. On aarch64 that is
 * a struct passed by pointer, so the arguments can be walked here. Unity uses the V form, so without
 * this the method arguments are lost and calls look like openFd("").
 */
// Framework dispatch currently accepts two reference/integer values. Decode
// the entire descriptor before reading V/A arguments and never guess objects.
// Floating-point return slots and non-V variadic FP calls remain a separate
// ABI task; reject FP arguments here instead of interpreting them as pointers.
bool FrameworkArguments(MethodRef *method, void *raw, bool array,
                        void **first, void **second,void **third=nullptr,void **fourth=nullptr)
{
    *first = *second = nullptr;
    if(third)*third=nullptr;
    if(fourth)*fourth=nullptr;
    if (!method) { Emit("JNI_ARGS_ERROR missing method"); return false; }
    rea_jni_args::Arguments args;
    std::string error;
    bool valid = false;
    if (array) {
        rea_jni_args::Descriptor descriptor;
        if (!rea_jni_args::Parse(method->signature.c_str(), descriptor, error)) {
            Emit("JNI_ARGS_ERROR %s.%s: %s", method->cls.c_str(), method->name.c_str(), error.c_str());
            return false;
        }
        std::vector<rea_jni_args::JValue> values(descriptor.parameters.size());
        if (!values.empty() && !raw) { Emit("JNI_ARGS_ERROR null A arguments"); return false; }
        for (size_t i=0;i<values.size();++i)
            memcpy(&values[i], static_cast<const unsigned char *>(raw)+i*8, 8);
        valid=rea_jni_args::DecodeA(method->signature.c_str(), values.data(), args, error);
    } else {
        valid=rea_jni_args::DecodeV(method->signature.c_str(), static_cast<va_list *>(raw), args, error);
    }
    if (!valid) { Emit("JNI_ARGS_ERROR %s.%s: %s", method->cls.c_str(), method->name.c_str(), error.c_str()); return false; }
    for (size_t i=0;i<args.size();++i) {
        const auto &arg=args[i]; intptr_t value=0;
        switch(arg.type) {
        case 'L': value=reinterpret_cast<intptr_t>(arg.value.l); break;
        case 'Z': value=arg.value.z; break;
        case 'B': value=arg.value.b; break;
        case 'C': value=arg.value.c; break;
        case 'S': value=arg.value.s; break;
        case 'I': value=arg.value.i; break;
        case 'J': value=static_cast<intptr_t>(arg.value.j); break;
        case 'F': {
            if(method->cls.find("SharedPreferences")==std::string::npos||(method->name!="putFloat"&&method->name!="getFloat")){
                Emit("JNI_ARGS_UNSUPPORTED_FP %s.%s",method->cls.c_str(),method->name.c_str());return false;
            }
            uint32_t bits;memcpy(&bits,&arg.value.f,4);value=bits;break;
        }
        default:
            Emit("JNI_ARGS_UNSUPPORTED_FP %s.%s %s", method->cls.c_str(), method->name.c_str(), method->signature.c_str());
            return false;
        }
        if (i==0) *first=reinterpret_cast<void *>(value);
        if (i==1) *second=reinterpret_cast<void *>(value);
        if (i==2 && third) *third=reinterpret_cast<void *>(value);
        if (i==3 && fourth) *fourth=reinterpret_cast<void *>(value);
    }
    return true;
}

FakeObject *NewObject(const std::string &cls)
{
    FakeObject *obj = new FakeObject();
    obj->cls = cls;
    std::lock_guard<std::mutex> lock(g_objectMutex);
    g_objects.push_back(obj);
    g_objectIndex.insert(obj);
    return obj;
}

MethodRef *NewMethod(const std::string &cls, const std::string &name, const std::string &sig,
                     bool isStatic, bool isField)
{
    // Canonical JNI IDs, with full equality after hashing (collisions stay safe).
    // This path is used repeatedly during original scene/asset initialization.
    uint64_t hash=14695981039346656037ull;
    for(const auto *part:{&cls,&name,&sig}){for(unsigned char c:*part){hash^=c;hash*=1099511628211ull;}hash^=0xff;hash*=1099511628211ull;}
    hash^=(isStatic?1:0)|(isField?2:0);hash*=1099511628211ull;
    std::lock_guard<std::mutex> lock(g_objectMutex);
    auto range=g_methodIndex.equal_range(hash);
    for(auto it=range.first;it!=range.second;++it){auto *existing=it->second;
        if (existing->cls == cls && existing->name == name && existing->signature == sig &&
            existing->isStatic == isStatic && existing->isField == isField) return existing;}
    MethodRef *m = new MethodRef();
    m->cls = cls;
    m->name = name;
    m->signature = sig;
    m->isStatic = isStatic;
    m->isField = isField;
    g_methods.push_back(m);g_methodIndex.emplace(hash,m);
    return m;
}

/* The asset root is where the game's assets/bin/Data tree is materialised on the device. */
std::string g_assetRoot = "/data/storage/el1/bundle/resources/rawfile/assets";
std::string g_systemLocale="en-US";
std::atomic<int> g_networkType{0};
std::mutex g_sdkStateMutex;
int g_sdkAge=-1,g_sdkRemaining=0;
std::string g_sdkToken;
bool g_sdkAllowed=false;
extern "C" void ReaSetSdkAuthState(int age,int remaining,const char *token,bool allowed){
    std::lock_guard<std::mutex> lock(g_sdkStateMutex);
    g_sdkAllowed=allowed&&token&&*token;
    g_sdkAge=g_sdkAllowed&&age>=0&&age<=18?age:-1;
    g_sdkRemaining=g_sdkAllowed?std::max(remaining,0):0;
    g_sdkToken=g_sdkAllowed?token:"";
}
FakeObject *g_currentActivity=nullptr;
std::mutex g_networkConfigMutex;
std::string g_systemProxy;
std::vector<FakeObject *> g_systemRoots;
thread_local FakeObject *g_pendingException=nullptr;
const char *g_packageName = "com.PigeonGames.Phigros";
bool g_traceJni = false;
bool g_manualIl2cppInit = false;
unsigned long g_unityBase = 0;
bool g_callChainRoot = false;
bool g_directRender = true;
bool g_gfxOnWorker = false;
bool g_fillAsyncSlot = false;
bool g_callSetWindow = false;
bool g_skipFrameSync = false;
static bool g_renderOnUnityThread = false;
static void *g_renderForWorker = nullptr;
static int g_traceJniCount = 0;
int g_openFdCount = 0;

std::string NormalizeAssetPath(const char *raw)
{
    std::string path = raw ? raw : "";
    while (!path.empty() && path[0] == '/') path.erase(path.begin());
    if (path.rfind("assets/", 0) == 0) path = path.substr(7);
    return path;
}

/* Implemented in il2cpp_lookup_hook.cpp: makes Unity's own il2cpp symbol lookups succeed. */
extern "C" bool ReaHookIl2cppLookup(unsigned long unityBase, void *il2cppHandle);

/* Implemented in pthread_abi.cpp: report scripting runtime initialisation outcomes. */
extern "C" void ReaSetRealIl2cppInit(void *init, void *domainGet);
extern "C" int rea_il2cpp_init(const char *domainName);
extern "C" void *rea_il2cpp_domain_get();
extern "C" void ReaSetRealIl2cppLookups(void *classFromName, void *classGetMethod);
extern "C" void *rea_il2cpp_class_from_name(void *image, const char *nameSpace, const char *name);
extern "C" void *rea_il2cpp_class_get_method_from_name(void *klass, const char *name, int argc);

/* Implemented in il2cpp_data.cpp: extracts global-metadata.dat and redirects il2cpp_set_data_dir. */
extern "C" void ReaPrepareIl2cppData();
extern "C" void ReaSetRealIl2cppDataDir(void *fn);
extern "C" void ReaIl2cppSetDataDir(const char *path);

/* Implemented in pthread_abi.cpp: redirects bionic-sized pthread objects to HarmonyOS ones. */
extern "C" int ReaPatchPthreadGOT(void *unityBase);

/* Zip-backed asset access, implemented in asset_zip.cpp. */
extern "C" bool ReaAssetOpen(const char *assetPath, int *outFd, int64_t *outOffset, int64_t *outLength);

/* Surface geometry published by surface.cpp once ArkUI has created the native window. */
extern "C" int32_t ReaShim_GetNativeWindowSize(int32_t *width, int32_t *height);

/* Opens a real file and describes it, so the engine can pread() through the returned fd. */
FakeObject *OpenAssetFd(const char *rawPath)
{
    std::string rel = NormalizeAssetPath(rawPath);

    int fd = -1;
    int64_t offset = 0;
    int64_t length = 0;
    if (!ReaAssetOpen(rel.c_str(), &fd, &offset, &length)) {
        g_openFdCount++;
        if (g_openFdCount <= 40 || (g_openFdCount % 200) == 0) {
            Emit("AssetManager.openFd(\"%s\") -> NOT FOUND", rel.c_str());
        }
        return nullptr;
    }

    FakeObject *afd = NewObject("android/content/res/AssetFileDescriptor");
    afd->fd = fd;
    afd->startOffset = offset;
    afd->length = length;
    afd->ownsFd = true;
    g_openFdCount++;
    if (g_openFdCount <= 40 || (g_openFdCount % 200) == 0) {
        Emit("AssetManager.openFd(\"%s\") -> fd=%d offset=%lld length=%lld (total %d)",
             rel.c_str(), fd, (long long)offset, (long long)length, g_openFdCount);
    }
    return afd;
}

/* ---- framework method dispatch ----------------------------------------------------------- */

void *DispatchObjectCall(FakeObject *obj, MethodRef *m, void *arg0, void *arg1,void *arg2=nullptr,void *arg3=nullptr);

/* Defined with the bootstrap below; the dispatcher records RegisterNatives through it. */
void CaptureRegistrations(FakeObject *clazz, void *methods, int count);
static bool HandleKeyboardCall(MethodRef *,const rea_jni_args::Arguments &);
static bool HandleNativeDialogCall(MethodRef *,const rea_jni_args::Arguments &);
static void VideoFrameAvailable(void *);

std::string ArgToString(void *arg)
{
    FakeObject *obj = (FakeObject *)arg;
    if (!obj) return "";
    auto it = obj->strs.find("value");
    return it != obj->strs.end() ? it->second : obj->cls;
}

} // namespace

/* ---- JNI surface ------------------------------------------------------------------------- */

extern "C" void *ReaJniDispatch(int index, void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h);

void *ReaFakeVmHandle();

#include "jni_table.inc"

namespace {

std::vector<int> g_slotsHit;

void NoteSlot(int index)
{
    for (int seen : g_slotsHit) {
        if (seen == index) return;
    }
    g_slotsHit.push_back(index);
    const char *name = (index >= 0 && index < 240) ? g_jni_names[index] : "?";
    Emit("JNI_SLOT %d (%s)", index, name);
}

/* Reads a slice of an asset at a given position. */
static long ReaReadAt(int fd, void *buffer, unsigned long length, long long position)
{
    if (lseek(fd, (off_t)position, SEEK_SET) < 0) return -1;
    return (long)read(fd, buffer, length);
}

/* Small helpers for the framework values the engine reads during start-up. */
static FakeObject *MakeStringObject(const char *value)
{
    FakeObject *s = NewObject("java/lang/String");
    s->strs["value"] = value;
    return s;
}

void *DispatchObjectCall(FakeObject *obj, MethodRef *m, void *arg0, void *arg1,void *arg2,void *arg3)
{
    /*
     * Start-up calls the engine makes once initialisation gets going. They are answered here without
     * depending on the exact class, because the engine reaches them through several wrappers.
     */
    if (m) {
        const std::string &name = m->name;
        if(obj&&obj->cls=="android/graphics/SurfaceTexture"){
            if(name=="<init>"){
                uint32_t texture=m->signature=="(Z)V"?0:(uint32_t)(uintptr_t)arg0;
                void *state=ReaVideoTextureCreate(texture,VideoFrameAvailable,obj);
                {std::lock_guard<std::mutex> lock(g_videoJniMutex);obj->refs["nativeTexture"]=state;}
                if(!state){g_pendingException=NewObject("java/lang/IllegalStateException");g_pendingException->strs["message"]="Could not create native video texture";}
                return nullptr;
            }
            void *state=nullptr;{std::lock_guard<std::mutex> lock(g_videoJniMutex);state=obj->refs["nativeTexture"];}
            if(name=="setOnFrameAvailableListener"){
                std::lock_guard<std::mutex> lock(g_videoJniMutex);obj->refs["frameListener"]=arg0;
                obj->refs["frameLooper"]=IsFakeObject(arg1)?static_cast<FakeObject *>(arg1)->refs["looper"]:nullptr;return nullptr;
            }
            if(name=="updateTexImage"){int result=ReaVideoTextureUpdate(state);if(result)Emit("MEDIA_TEXTURE_UPDATE result=%d",result);return nullptr;}
            if(name=="getTimestamp")return (void *)(intptr_t)ReaVideoTextureTimestamp(state);
            if(name=="getTransformMatrix"){
                if(IsFakeObject(arg0)){auto *array=static_cast<FakeObject *>(arg0);float matrix[16]{};for(int i=0;i<16;i+=5)matrix[i]=1;
                    ReaVideoTextureTransform(state,matrix);array->strs["bytes"].assign((const char *)matrix,sizeof(matrix));array->ints["length"]=16;}
                return nullptr;
            }
            if(name=="setDefaultBufferSize"){ReaVideoTextureSize(state,(intptr_t)arg0,(intptr_t)arg1);return nullptr;}
            if(name=="attachToGLContext"){ReaVideoTextureAttach(state,(uintptr_t)arg0);return nullptr;}
            if(name=="detachFromGLContext"){ReaVideoTextureDetach(state);return nullptr;}
            if(name=="release"){ReaVideoTextureDestroy(state);return nullptr;}
            if(name=="isReleased")return (void *)(intptr_t)(ReaVideoTextureWindow(state)?0:1);
        }
        if(obj&&obj->cls=="android/view/Surface"){
            if(name=="<init>"&&m->signature=="(Landroid/graphics/SurfaceTexture;)V"){
                std::lock_guard<std::mutex> lock(g_videoJniMutex);obj->refs["surfaceTexture"]=arg0;obj->ints["videoSurface"]=1;return nullptr;
            }
            if(name=="release"){std::lock_guard<std::mutex> lock(g_videoJniMutex);obj->ints["released"]=1;return nullptr;}
        }
        if(name=="getClass"&&obj){auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]=obj->cls;return clazz;}
        if(obj&&obj->preferences){
            if(name=="edit"){
                auto *editor=NewObject("android/content/SharedPreferences$Editor");editor->preferenceEditor=std::make_shared<rea_prefs::Editor>();
                editor->preferenceEditor->store=obj->preferences;return editor;
            }
            if(name=="getAll"){
                auto *map=NewObject("java/util/HashMap");
                for(const auto &item:rea_prefs::Snapshot(obj->preferences)){
                    auto *entry=NewObject("java/util/Map$Entry");entry->refs["key"]=MakeStringObject(item.first.c_str());
                    const auto &value=item.second;FakeObject *boxed=nullptr;
                    if(value.type==rea_prefs::Type::String)boxed=MakeStringObject(value.text.c_str());
                    else if(value.type==rea_prefs::Type::Float){boxed=NewObject("java/lang/Float");boxed->numbers["value"]=value.number;}
                    else {boxed=NewObject(value.type==rea_prefs::Type::Bool?"java/lang/Boolean":value.type==rea_prefs::Type::Long?"java/lang/Long":"java/lang/Integer");boxed->ints["value"]=value.integer;}
                    entry->refs["value"]=boxed;map->items.push_back(entry);
                }
                Emit("PREFS_GET_ALL entries=%zu",map->items.size());return map;
            }
            rea_prefs::Value value;bool found=rea_prefs::Read(obj->preferences,ArgToString(arg0),value);
            if(name=="contains")return (void *)(intptr_t)found;
            if(name=="getString")return found&&value.type==rea_prefs::Type::String?MakeStringObject(value.text.c_str()):arg1;
            if(name=="getInt"||name=="getLong"||name=="getBoolean")return found?(void *)(intptr_t)value.integer:arg1;
        }
        if(obj&&obj->preferenceEditor){
            auto editor=obj->preferenceEditor;
            if(name=="commit"||name=="apply")return (void *)(intptr_t)rea_prefs::Apply(editor);
            std::lock_guard<std::mutex> lock(editor->mutex);std::string key=ArgToString(arg0);
            if(name=="clear"){editor->clear=true;return obj;}
            if(name=="remove"||(name=="putString"&&!arg1)){editor->pending.erase(key);editor->removed.insert(key);return obj;}
            rea_prefs::Value value;bool put=true;
            if(name=="putString"){value.type=rea_prefs::Type::String;value.text=ArgToString(arg1);}
            else if(name=="putInt"){value.type=rea_prefs::Type::Int;value.integer=(int32_t)(intptr_t)arg1;}
            else if(name=="putLong"){value.type=rea_prefs::Type::Long;value.integer=(int64_t)(intptr_t)arg1;}
            else if(name=="putBoolean"){value.type=rea_prefs::Type::Bool;value.integer=arg1?1:0;}
            else if(name=="putFloat"){value.type=rea_prefs::Type::Float;uint32_t bits=(uintptr_t)arg1;memcpy(&value.number,&bits,4);}
            else put=false;
            if(put){editor->removed.erase(key);editor->pending[key]=std::move(value);return obj;}
        }
        if(obj&&obj->cls=="java/util/HashMap"){
            if(name=="entrySet"){auto *set=NewObject("java/util/Set");set->items=obj->items;return set;}
            if(name=="keySet"||name=="values"){
                auto *set=NewObject("java/util/Set");for(void *raw:obj->items){auto *entry=static_cast<FakeObject *>(raw);set->items.push_back(entry->refs[name=="keySet"?"key":"value"]);}return set;
            }
            if(name=="size")return (void *)(intptr_t)obj->items.size();
            if(name=="isEmpty")return (void *)(intptr_t)obj->items.empty();
            if(name=="get"||name=="containsKey"){
                std::string key=ArgToString(arg0);for(void *raw:obj->items){auto *entry=static_cast<FakeObject *>(raw);if(ArgToString(entry->refs["key"])==key)return name=="get"?entry->refs["value"]:(void *)1;}
                return nullptr;
            }
        }
        if(obj&&obj->cls=="java/util/Set"){
            if(name=="iterator"){auto *iterator=NewObject("java/util/Iterator");iterator->items=obj->items;return iterator;}
            if(name=="size")return (void *)(intptr_t)obj->items.size();
            if(name=="isEmpty")return (void *)(intptr_t)obj->items.empty();
            if(name=="toArray"){auto *array=NewObject("[Ljava/lang/Object;");array->ints["length"]=obj->items.size();for(size_t i=0;i<obj->items.size();++i)array->refs["["+std::to_string(i)+"]"]=obj->items[i];return array;}
        }
        if(obj&&obj->cls=="java/util/Iterator"){
            size_t index=obj->ints["position"];
            if(name=="hasNext")return (void *)(intptr_t)(index<obj->items.size());
            if(name=="next"){if(index>=obj->items.size())return nullptr;obj->ints["position"]=index+1;return obj->items[index];}
        }
        if(obj&&obj->cls=="java/util/Map$Entry"){
            if(name=="getKey")return obj->refs["key"];if(name=="getValue")return obj->refs["value"];
        }
        if(m->cls=="com/unity3d/player/ReflectionHelper"){
            if(name=="newProxyInstance" && m->signature=="(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;"){
                auto *proxy=NewObject("com/unity3d/player/ManagedProxy");
                proxy->ints["gchandle"]=(int64_t)(uintptr_t)arg1;
                proxy->refs["interface"]=arg2;
                Emit("SDK_PROXY_CREATED interface=%s",IsFakeObject(arg2)?static_cast<FakeObject *>(arg2)->strs["name"].c_str():"unknown");
                return proxy;
            }
            if(name=="getMethodID"||name=="getFieldID"||name=="getConstructorID"){
                if(!IsFakeObject(arg0))return nullptr;
                auto *clazz=static_cast<FakeObject *>(arg0);
                bool constructor=name=="getConstructorID",field=name=="getFieldID";
                auto member=constructor?std::string("<init>"):ArgToString(arg1);
                auto signature=ArgToString(constructor?arg1:arg2);
                // Unity's reflection helper accepts Class.getName()'s dotted binary names.
                // JNI's ABI decoder requires the normalized descriptor returned by that helper.
                std::replace(signature.begin(),signature.end(),'.','/');
                if(signature.empty()||member.empty())return nullptr;
                auto *reflected=NewObject(field?"java/lang/reflect/Field":constructor?"java/lang/reflect/Constructor":"java/lang/reflect/Method");
                auto *method=NewMethod(clazz->strs["name"],member,signature,constructor?false:arg3!=nullptr,field);
                reflected->refs[field?"field":"method"]=method;
                Emit("JNI_REFLECTION %s.%s %s static=%d",method->cls.c_str(),member.c_str(),signature.c_str(),method->isStatic?1:0);
                return reflected;
            }
            if(name=="getFieldSignature"&&IsFakeObject(arg0)){
                auto *field=static_cast<MethodRef *>(static_cast<FakeObject *>(arg0)->refs["field"]);
                return field?MakeStringObject(field->signature.c_str()):nullptr;
            }
        }
        if(obj&&obj->cls=="java/lang/reflect/Field"&&name=="getDeclaringClass"){
            auto *field=static_cast<MethodRef *>(obj->refs["field"]);
            if(!field)return nullptr;
            auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]=field->cls;return clazz;
        }
        if(m->cls=="com/tds/common/bridge/Bridge" || (obj&&obj->cls=="com/tds/common/bridge/Bridge")){
            if(name=="getInstance") {static auto *bridge=NewObject("com/tds/common/bridge/Bridge");return bridge;}
            if(name=="init") {if(obj)obj->refs["activity"]=arg0;return nullptr;}
            if(name=="register") {Emit("SDK_SERVICE_REGISTERED class=%s",IsFakeObject(arg0)?static_cast<FakeObject *>(arg0)->strs["name"].c_str():"unknown");return nullptr;}
            if(name=="callHandler"||name=="registerHandler"){
                void *proxy=name=="registerHandler"&&IsFakeObject(arg1)?arg1:nullptr;
                if(!ReaSdkEnqueue(ArgToString(arg0),proxy))Emit("SDK_REQUEST_REJECTED invalid or queue full");
                return nullptr;
            }
        }
        if(m->cls=="com/tapsdk/antiaddiction/AntiAddictionKit"){
            std::lock_guard<std::mutex> lock(g_sdkStateMutex);
            if(name=="currentUserAgeLimit")return (void *)(intptr_t)g_sdkAge;
            if(name=="currentUserRemainingTime")return (void *)(intptr_t)g_sdkRemaining;
            if(name=="currentToken")return g_sdkToken.empty()?nullptr:MakeStringObject(g_sdkToken.c_str());
            if(name=="isNotAdult")return (void *)(intptr_t)(!g_sdkAllowed||g_sdkAge<18);
        }
        if (name == "<init>") {
            if (obj && m->signature == "(J)V") obj->ints["cookie"] = (int64_t)(intptr_t)arg0;
            if (obj && m->cls == "java/lang/Thread") obj->refs["runnable"] = arg0;
            if (obj && m->cls == "android/os/HandlerThread") {
                auto *looper = NewObject("android/os/Looper");
                looper->loop = std::make_shared<JavaLoop>();
                obj->refs["looper"] = looper;
                obj->strs["threadName"] = ArgToString(arg0);
            }
            if (obj && m->cls == "android/os/Handler") {
                obj->refs["looper"] = m->signature == "()V" ? g_threadLooper : arg0;
                obj->refs["callback"] = arg1;
            }
            if (obj && m->cls == "java/lang/String") {
                if (m->signature == "([BLjava/lang/String;)V" && IsFakeObject(arg0))
                    obj->strs["value"] = static_cast<FakeObject *>(arg0)->strs["bytes"];
                else obj->strs["value"] = ArgToString(arg0);
            }
            if (obj && m->cls == "java/io/File") obj->strs["path"] = ArgToString(arg0);
            // String conversion is a hot path during save/result/chapter loading.
            // Keep detailed tracing opt-in instead of doing IPC logging per conversion.
            if(m->cls=="java/lang/String"){
                uint64_t count=g_stringConstructions.fetch_add(1,std::memory_order_relaxed)+1;
                if(g_traceJni||count<=4)Emit("JNI_CONSTRUCT %s %s",m->cls.c_str(),m->signature.c_str());
            }else Emit("JNI_CONSTRUCT %s %s", m->cls.c_str(), m->signature.c_str());
            return nullptr;
        }

        /* Threads, looper and storage state are enough to satisfy the checks. */
        if (name == "setThreadPriority") return nullptr;
        if(m->cls=="java/lang/System"&&(name=="nanoTime"||name=="currentTimeMillis")){
            timespec now{};clock_gettime(name=="nanoTime"?CLOCK_MONOTONIC:CLOCK_REALTIME,&now);
            int64_t value=name=="nanoTime"?(int64_t)now.tv_sec*1000000000+now.tv_nsec:(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
            return (void *)(intptr_t)value;
        }
        if(m->cls=="java/lang/Runtime"){
            if(name=="getRuntime")return NewObject("java/lang/Runtime");
            if(name=="availableProcessors")return (void *)(intptr_t)sysconf(_SC_NPROCESSORS_ONLN);
        }
        if(m->cls=="com/unity3d/player/UnityPlayer"&&name=="getNetworkConnectivity")return (void *)(intptr_t)g_networkType.load();
        if(m->cls=="com/unity3d/player/UnityPlayer"&&name=="getNetworkProxySettings"){
            std::lock_guard<std::mutex> lock(g_networkConfigMutex);
            return g_systemProxy.empty()?nullptr:MakeStringObject(g_systemProxy.c_str());
        }
        if(m->cls=="javax/net/ssl/TrustManagerFactory"){
            if(name=="getDefaultAlgorithm")return MakeStringObject("PKIX");
            if(name=="getInstance")return NewObject("javax/net/ssl/TrustManagerFactory");
            if(name=="init")return nullptr;
            if(name=="getTrustManagers"){
                auto *array=NewObject("[Ljavax/net/ssl/TrustManager;");array->ints["length"]=1;
                array->refs["[0]"]=NewObject("javax/net/ssl/X509TrustManager");return array;
            }
        }
        if(m->cls=="javax/net/ssl/X509TrustManager"|| (obj&&obj->cls=="javax/net/ssl/X509TrustManager")){
            if(name=="getAcceptedIssuers"){
                auto *array=NewObject("[Ljava/security/cert/X509Certificate;");
                std::lock_guard<std::mutex> lock(g_networkConfigMutex);array->ints["length"]=g_systemRoots.size();
                for(size_t i=0;i<g_systemRoots.size();++i)array->refs["["+std::to_string(i)+"]"]=g_systemRoots[i];
                Emit("TLS_SYSTEM_ROOTS count=%zu (native TLS keeps certificate verification)",g_systemRoots.size());return array;
            }
            if(name=="checkServerTrusted"||name=="checkClientTrusted"){
                // This facade only supplies real trust anchors to Unity's native
                // TLS verifier. Never turn an unimplemented Java check into trust-all.
                g_pendingException=NewObject("java/security/cert/CertificateException");
                g_pendingException->strs["message"]="Java chain validation is unsupported; use native TLS verification";
                return nullptr;
            }
        }
        if(obj&&obj->cls=="java/security/cert/X509Certificate"&&name=="getEncoded"){
            auto *bytes=NewObject("[B");bytes->strs["bytes"]=obj->strs["der"];bytes->ints["length"]=bytes->strs["bytes"].size();return bytes;
        }
        if (name == "getExternalStorageState") return MakeStringObject("mounted");
        if (name == "getMainLooper") return GetMainJavaLoop();
        if (name == "playCoreApiMissing") return (void *)(intptr_t)0;
        if (name == "init" && obj &&
            (obj->cls.find("Choreographer") != std::string::npos ||
             obj->cls.find("java/lang/Class") != std::string::npos))
            return nullptr;
        if (name == "newInterfaceProxy") {
            Emit("JNI_PROXY_REQUEST %s %s cookie=0x%lx", m->cls.c_str(), m->signature.c_str(), (unsigned long)(uintptr_t)arg0);
            if (m->cls != "bitter/jnibridge/JNIBridge") return nullptr;
            auto *proxy = NewObject("bitter/jnibridge/Proxy");
            proxy->ints["cookie"] = (intptr_t)arg0;
            proxy->refs["interfaces"] = arg1;
            return proxy;
        }
        if (name == "disableInterfaceProxy") {
            if (IsFakeObject(arg0)) static_cast<FakeObject *>(arg0)->ints["cookie"] = 0;
            return nullptr;
        }
        if (name == "initializeGoogleAr") return nullptr;
        if(m->cls=="android/os/Process"&&name=="myPid")return (void *)(intptr_t)getpid();
        if(m->cls=="android/os/Process"&&name=="killProcess"){
            if((intptr_t)arg0==getpid())ReaRequestPlayerExit("Process.killProcess(self)");
            else Emit("PLAYER_EXIT_REJECT target is not this process");return nullptr;
        }
        if(m->cls=="java/lang/System"&&name=="exit"){ReaRequestPlayerExit("System.exit");return nullptr;}
        if((name=="kill"||name=="finish"||name=="quit")&&
           (m->cls=="com/unity3d/player/UnityPlayer"||obj==g_currentActivity)){
            ReaRequestPlayerExit(name.c_str());return nullptr;
        }
        if (name == "queueGLThreadEvent" || name == "queueEvent") {
            if (IsFakeObject(arg0)) QueueUnityJob(static_cast<FakeObject *>(arg0),name=="queueGLThreadEvent");
            return nullptr;
        }
        if (name == "runOnUiThread") {
            QueueJavaTask(GetMainJavaLoop(), static_cast<FakeObject *>(arg0), "run");
            return nullptr;
        }
        if (name == "getSharedPreferences") {
            auto store=rea_prefs::Open(ArgToString(arg0));
            if(!store){g_pendingException=NewObject("java/io/IOException");g_pendingException->strs["message"]="Cannot safely read SharedPreferences";return nullptr;}
            FakeObject *prefs = NewObject("android/content/SharedPreferences");prefs->preferences=store;
            return prefs;
        }
        if (name == "getObbDirs") {
            FakeObject *array = NewObject("[Ljava/io/File;");
            FakeObject *dir = NewObject("java/io/File");
            dir->strs["path"] = "/data/storage/el2/base/files";
            array->refs["[0]"] = dir;
            array->ints["length"] = 1;
            return array;
        }
        if (name == "getPackageInfo") return NewObject("android/content/pm/PackageInfo");
        if (name == "findLibrary") {
            auto library=ArgToString(arg0);
            if(library.empty())return nullptr;
            if(library.rfind("lib",0)!=0)library="lib"+library;
            if(library.size()<3||library.substr(library.size()-3)!=".so")library+=".so";
            Emit("JNI_FIND_LIBRARY %s",library.c_str());return MakeStringObject(library.c_str());
        }
        if(m->cls=="java/util/Locale"){
            if(name=="getDefault")return NewObject("java/util/Locale");
            auto separator=g_systemLocale.find_first_of("-_");
            auto language=g_systemLocale.substr(0,separator);
            auto last=g_systemLocale.find_last_of("-_");
            auto country=last==std::string::npos?std::string():g_systemLocale.substr(last+1);
            if(name=="getLanguage")return MakeStringObject(language.c_str());
            if(name=="getCountry")return MakeStringObject(country.c_str());
            if(name=="toLanguageTag"||name=="toString")return MakeStringObject(g_systemLocale.c_str());
        }
        if(m->cls=="android/content/res/Configuration"&&name=="getLocales")return NewObject("android/os/LocaleList");
        if(m->cls=="android/os/LocaleList"){
            if(name=="size")return (void *)1;
            if(name=="get")return (intptr_t)arg0==0?NewObject("java/util/Locale"):nullptr;
            if(name=="getDefault")return NewObject("android/os/LocaleList");
        }
        if(m->cls=="org/fmod/FMODAudioDevice"){
            if(name=="start"){ReaFmodStart(g_unityBase,obj);return nullptr;}
            if(name=="stop"||name=="close"){ReaFmodStop();return nullptr;}
            if(name=="isRunning")return (void *)(intptr_t)ReaFmodRunning();
        }
        if(name=="getDevices"&&m->signature=="(I)[Landroid/media/AudioDeviceInfo;"){
            // There are no Android AudioDeviceInfo route IDs on this host.
            auto *array=NewObject("[Landroid/media/AudioDeviceInfo;");array->ints["length"]=0;return array;
        }
        if(m->cls=="com/Exceed7/NativeAudio/NativeAudio"&&name=="GetDeviceAudioInformation"){
            // These are the configured Harmony mixer parameters (see ReaSLES),
            // not invented Android hardware IDs or Android feature declarations.
            auto *information=NewObject("com/Exceed7/NativeAudio/NativeAudio$DeviceAudioInformation");
            information->ints["nativeSamplingRate"]=48000;information->ints["optimalBufferSize"]=192;
            information->ints["lowLatencyFeature"]=0;information->ints["proAudioFeature"]=0;
            auto *devices=NewObject("[Landroid/media/AudioDeviceInfo;");devices->ints["length"]=0;
            information->refs["outputDevices"]=devices;
            Emit("E7_DEVICE_INFO rate=48000 buffer=192 androidDeviceIds=0 (Harmony default route)");
            return information;
        }
        if(m->cls=="com/Exceed7/NativeAudio/NativeAudio"&&name=="Initialize"){
            static void *plugin=nullptr;
            if(!plugin){
                plugin=dlopen("libnativeaudioe7.so",RTLD_NOW|RTLD_LOCAL);
                if(plugin){Dl_info info{};auto *symbol=dlsym(plugin,"sendByteArray");if(symbol&&dladdr(symbol,&info))ReaPatchPthreadGOT(info.dli_fbase);}
            }
            auto initialize=plugin?(void (*)(void *,void *,int,int,int))dlsym(plugin,"Java_com_Exceed7_NativeAudio_NativeAudio_initialize"):nullptr;
            if(!initialize){Emit("E7_INITIALIZE_FAILED %s",dlerror());return (void *)-1;}
            int frames=192,minimum=(intptr_t)arg1,players=(intptr_t)arg0;
            if(minimum!=-1&&frames<minimum)frames=(minimum/frames+1)*frames;
            if(players<1||players>64)return (void *)-1;
            initialize(ReaFrameworkEnv(),obj,48000,frames,players);
            Emit("E7_INITIALIZE rate=48000 buffer=%d players=%d preserve=%d",frames,players,arg2?1:0);
            return nullptr;
        }
        if(m->cls=="com/Exceed7/NativeAudio/NativeAudio"&&name=="Dispose"){
            auto *plugin=dlopen("libnativeaudioe7.so",RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
            auto dispose=plugin?(void (*)(void *,void *))dlsym(plugin,"Java_com_Exceed7_NativeAudio_NativeAudio_disposeIfAllocated"):nullptr;
            if(dispose)dispose(ReaFrameworkEnv(),obj);
            if(plugin)dlclose(plugin);
            return nullptr;
        }

        /* Display metrics: report the panel the surface already uses. */
        if (name == "getWidth" || name == "getHeight") {
            int32_t width=1920,height=1080;ReaShim_GetNativeWindowSize(&width,&height);
            return (void *)(intptr_t)(name=="getWidth"?width:height);
        }
        if (name == "getRotation") return (void *)(intptr_t)0;
        if (name == "getDisplayId") return (void *)(intptr_t)0;
        if (name == "isHdr") return (void *)(intptr_t)0;
        if (name == "isWideColorGamut") return (void *)(intptr_t)0;
        if (name == "getHdrCapabilities") return NewObject("android/view/Display$HdrCapabilities");
        if (name == "getSupportedHdrTypes") return NewObject("[I");
        if (name == "getRealMetrics") {
            /* DisplayMetrics: width/height live at the start of the object. */
            FakeObject *metrics = (FakeObject *)arg0;
            if (metrics) {
                int32_t width=1920,height=1080;ReaShim_GetNativeWindowSize(&width,&height);
                metrics->ints["widthPixels"] = width;
                metrics->ints["heightPixels"] = height;
                metrics->ints["densityDpi"] = 320;
                metrics->ints["density"] = 2;
                metrics->ints["scaledDensity"] = 2;
                metrics->ints["xdpi"] = 320;
                metrics->ints["ydpi"] = 320;
            }
            return nullptr;
        }

        /* Where the receiver is a Context-derived fake, its preference-style results are answered here. */
        const bool contextLike =
            obj && (obj->cls.find("Context") != std::string::npos ||
                    obj->cls.find("SharedPreferences") != std::string::npos ||
                    obj->cls == "android/os/Bundle");
        const bool preferenceCall=contextLike && m->signature.rfind("(Ljava/lang/String;",0)==0;
        // Context.getString(int resourceId) is not SharedPreferences.getString(String,...).
        if (contextLike && name=="getString" && !preferenceCall) return MakeStringObject("");

        /* SharedPreferences/Bundle: missing keys preserve the caller's typed default. */
        if (preferenceCall && name == "getString") {
            auto value=obj->strs.find(ArgToString(arg0));
            return value==obj->strs.end() ? arg1 : MakeStringObject(value->second.c_str());
        }
        if (preferenceCall && (name == "getBoolean" || name == "getInt" || name == "getLong")) {
            auto value=obj->ints.find(ArgToString(arg0));
            intptr_t result=value==obj->ints.end() ? reinterpret_cast<intptr_t>(arg1) : static_cast<intptr_t>(value->second);
            return reinterpret_cast<void *>(name=="getBoolean" ? (result ? 1 : 0) : result);
        }
        if (preferenceCall && name == "getFloat") return nullptr; // requires typed FP return slots
        if (preferenceCall && name == "putString") { obj->strs[ArgToString(arg0)]=ArgToString(arg1); return obj; }
        if (preferenceCall && (name == "putBoolean" || name == "putLong")) {
            obj->ints[ArgToString(arg0)]=reinterpret_cast<intptr_t>(arg1); return obj;
        }
        if (name == "putFloat") return obj; // not implemented by the pointer-return ABI
        if (preferenceCall && name == "contains") {
            std::string key=ArgToString(arg0);
            return reinterpret_cast<void *>(static_cast<intptr_t>(obj->strs.count(key)||obj->ints.count(key)));
        }

        /* Empty map iteration for getAll()/entrySet(). */
        if (name == "getAll" || name == "entrySet" || name == "keySet" || name == "values")
            return NewObject("java/util/Map");
        if (name == "iterator") return NewObject("java/util/Iterator");
        if (name == "hasNext") return (void *)(intptr_t)0;
        if (name == "next") return nullptr;

        /* The platform frame callback: remember it and let the driver thread fire it. */
        if ((name == "postFrameCallback" || name == "postFrameCallbackDelayed") &&
            obj && obj->cls == "android/view/Choreographer") {
            QueueJavaTask(static_cast<FakeObject *>(obj->refs["looper"]),
                          static_cast<FakeObject *>(arg0), "doFrame", nullptr,
                          name == "postFrameCallbackDelayed" ? (intptr_t)arg1 : 0);
            return nullptr;
        }

        /* Location and display services: report "nothing available" rather than nulls. */
        if (name == "isProviderEnabled" || name == "isLocationEnabled") return (void *)(intptr_t)0;
        if (name == "getLastKnownLocation" || name == "getProviders") return nullptr;
        if (name == "getDisplays") {
            FakeObject *array = NewObject("[Landroid/view/Display;");
            FakeObject *display = NewObject("android/view/Display");
            array->refs["[0]"] = display;
            array->ints["length"] = 1;
            return array;
        }
        if (name == "registerDisplayListener" || name == "unregisterDisplayListener" ||
            name == "requestLocationUpdates" || name == "removeUpdates")
            return nullptr;
        if (name == "getDefaultDisplay") return NewObject("android/view/Display");

        /* Paths: the engine builds its data roots from these, so they must be real strings. */
        if (name == "getFilesDir" || name == "getExternalFilesDir" || name == "getObbDir" ||
            name == "getCacheDir" || name == "getExternalCacheDir") {
            FakeObject *dir = NewObject("java/io/File");
            dir->strs["path"] = "/data/storage/el2/base/files";
            return dir;
        }
        if (name == "getPackageCodePath") return MakeStringObject("/data/storage/el2/base/files/rea-original-phigros.apk");
        if (name == "getPackageName") return MakeStringObject("com.PigeonGames.Phigros");
        if (name == "getPath") return MakeStringObject("/data/storage/el2/base/files");

        /* Display hand-off and the remaining UnityPlayer helpers. */
        if (name == "getDisplay") return NewObject("android/view/Display");
        if (name == "getClassLoader") return NewObject("java/lang/ClassLoader");
        if (name == "getClass" && obj) {auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]=obj->cls;return clazz;}
        if (name == "init" && obj && obj->cls.find("Choreographer") != std::string::npos) return nullptr;

        /* Play asset-pack status: report nothing registered. */
        if (name == "registerDownloadStatusListener" || name == "getAssetPackState") return nullptr;

        /* SharedPreferences accessors. */
        if (preferenceCall && name == "putInt") {
            if (obj) obj->ints[ArgToString(arg0)] = (int64_t)(intptr_t)arg1;
            return obj;
        }
        if (contextLike && name == "edit") return obj;
        if (contextLike && (name == "apply" || name == "commit")) return (void *)(intptr_t)1;
    }

    if (g_traceJni && g_traceJniCount < 120 && obj && m) {
        if (m->name == "getSystemService") {
            std::string service = ArgToString(arg0);
            Emit("JNI CALL %s.getSystemService(%s)", obj->cls.c_str(),
                 service.empty() ? "?" : service.c_str());
        } else if (obj->cls.find("AssetManager") != std::string::npos) {
            FakeObject *argument = (FakeObject *)arg0;
            const char *path = (argument && !argument->strs.empty()) ? argument->strs.begin()->second.c_str()
                                                                     : "?";
            Emit("JNI CALL %s.%s(%s)", obj->cls.c_str(), m->name.c_str(), path);
        } else {
            Emit("JNI CALL %s.%s", obj->cls.c_str(), m->name.c_str());
        }
        g_traceJniCount++;
    }
    if (!obj || !m) return nullptr;
    const std::string &cls = m->isStatic ? m->cls : obj->cls;
    const std::string &name = m->name;

    if (cls == "android/view/MotionEvent") {
        if (name == "getSource") return (void *)(intptr_t)0x1002;
        if (name == "getPointerCount") return (void *)(intptr_t)obj->touchPoints.size();
        if (name == "getAction" || name == "getActionMasked" || name == "getActionIndex") {
            int action=(int)obj->ints["action"];
            return (void *)(intptr_t)(name=="getAction"?action:name=="getActionMasked"?action&255:action>>8);
        }
        if (name == "getPointerId") {
            size_t index=(uintptr_t)arg0;
            return (void *)(intptr_t)(index<obj->touchPoints.size()?obj->touchPoints[index].id:-1);
        }
        if (name == "findPointerIndex") {
            for (size_t i=0;i<obj->touchPoints.size();++i) if(obj->touchPoints[i].id==(intptr_t)arg0)return (void *)(intptr_t)i;
            return (void *)(intptr_t)-1;
        }
        if (name == "getEventTime" || name == "getDownTime") return (void *)(intptr_t)obj->ints[name];
        if (name == "getToolType") return (void *)1;
        if (name == "getHistorySize" || name == "getButtonState" || name == "getMetaState" || name == "getFlags" ||
            name == "getDeviceId" || name == "getEdgeFlags" || name == "getDisplayId") return nullptr;
        if (name == "obtain") return arg0;
        if (name == "recycle") return nullptr;
    }

    if (cls == "java/lang/String") {
        const std::string &value = obj->strs["value"];
        if (name == "length") {
            try { return (void *)(intptr_t)std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(value).size(); }
            catch (...) { return nullptr; }
        }
        if (name == "toString") return obj;
        if (name == "getBytes") {
            auto *array = NewObject("[B"); array->strs["bytes"] = value; return array;
        }
        if (name == "isEmpty") return (void *)(intptr_t)value.empty();
    }
    if (cls == "android/net/Uri" && (name == "encode" || name == "decode")) {
        if (!arg0) return nullptr;
        std::string input = ArgToString(arg0), output;
        const char *hex = "0123456789ABCDEF";
        std::string allow = name == "encode" && arg1 ? ArgToString(arg1) : "";
        auto nibble = [](char c) -> int { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
        for (size_t i=0;i<input.size();++i) {
            unsigned char c = input[i];
            if (name == "encode") {
                bool keep = (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
                            (c && strchr("_-!.~'()*", c)) || allow.find(c) != std::string::npos;
                if (keep) output += c;
                else { output += '%'; output += hex[c>>4]; output += hex[c&15]; }
            } else if (c == '%') {
                if (i+2<input.size() && nibble(input[i+1])>=0 && nibble(input[i+2])>=0) {
                    output += (char)((nibble(input[i+1])<<4)|nibble(input[i+2])); i+=2;
                } else output += "\xef\xbf\xbd";
            } else output += c; // Uri.decode keeps '+'; it is not a form decoder.
        }
        auto *result = NewObject("java/lang/String"); result->strs["value"] = output; return result;
    }
    if (cls == "java/lang/Integer" && name == "parseInt") return (void *)(intptr_t)strtol(ArgToString(arg0).c_str(), nullptr, 10);
    if (cls == "android/content/pm/PackageManager" && name == "hasSystemFeature") return nullptr;
    if (cls == "java/lang/StringBuilder") {
        if (name == "append") { obj->strs["value"] += ArgToString(arg0); return obj; }
        if (name == "toString") return MakeStringObject(obj->strs["value"].c_str());
    }
    if (cls == "android/os/Looper") {
        if (name == "prepare") { PrepareJavaLoop(); return nullptr; }
        if (name == "myLooper") return g_threadLooper;
        if (name == "loop") { RunJavaLoop(g_threadLooper); return nullptr; }
        if (name == "quit" || name == "quitSafely") {
            if (obj->loop) { std::lock_guard<std::mutex> lock(obj->loop->mutex); obj->loop->quit = true; obj->loop->changed.notify_all(); }
            return nullptr;
        }
    }
    if (cls == "java/lang/Thread") {
        if (name == "start") {
            pthread_t thread;
            int result = pthread_create(&thread, nullptr, JavaThreadBody, obj);
            if (result == 0) pthread_detach(thread);
            Emit("JAVA_THREAD_START result=%d", result);
            return nullptr;
        }
        if (name == "setName" || name == "setDaemon") return nullptr;
    }
    if (cls == "android/os/HandlerThread") {
        if (name == "getLooper") return obj->refs["looper"];
        if (name == "start") {
            pthread_t thread;
            int result = pthread_create(&thread, nullptr, [](void *raw) -> void * {
                auto *handlerThread = static_cast<FakeObject *>(raw);
                g_threadLooper = static_cast<FakeObject *>(handlerThread->refs["looper"]);
                auto threadName = handlerThread->strs["threadName"].substr(0,15);
                pthread_setname_np(pthread_self(), threadName.c_str());
                Emit("JAVA_HANDLER_THREAD tid=%d name=%s", (int)syscall(SYS_gettid), threadName.c_str());
                RunJavaLoop(g_threadLooper);
                return nullptr;
            }, obj);
            if (result == 0) pthread_detach(thread);
            Emit("JAVA_HANDLER_THREAD_START result=%d", result);
            return nullptr;
        }
    }
    if (cls == "android/os/Handler") {
        if (name == "obtainMessage") {
            auto *message = NewObject("android/os/Message");
            message->ints["what"] = (intptr_t)arg0;
            if(m->signature=="(III)Landroid/os/Message;"||m->signature=="(IIILjava/lang/Object;)Landroid/os/Message;"){
                message->ints["arg1"]=(int32_t)(intptr_t)arg1;message->ints["arg2"]=(int32_t)(intptr_t)arg2;
                if(m->signature=="(IIILjava/lang/Object;)Landroid/os/Message;")message->refs["obj"]=arg3;
            }else if(m->signature=="(ILjava/lang/Object;)Landroid/os/Message;")message->refs["obj"]=arg1;
            message->refs["target"] = obj;
            return message;
        }
        if(name=="sendMessage"||name=="sendMessageDelayed"||name=="sendMessageAtFrontOfQueue"){
            if(!IsFakeObject(arg0))return nullptr;auto *message=static_cast<FakeObject *>(arg0);message->refs["target"]=obj;
            QueueJavaTask(static_cast<FakeObject *>(obj->refs["looper"]),static_cast<FakeObject *>(obj->refs["callback"]),"handleMessage",message,
                          name=="sendMessageDelayed"?(intptr_t)arg1:0);return (void *)1;
        }
        if (name == "post" || name == "postDelayed") {
            QueueJavaTask(static_cast<FakeObject *>(obj->refs["looper"]), static_cast<FakeObject *>(arg0), "run", nullptr,
                          name == "postDelayed" ? (intptr_t)arg1 : 0);
            return (void *)1;
        }
    }
    if (cls == "android/os/Message" && name == "sendToTarget") {
        auto *target = static_cast<FakeObject *>(obj->refs["target"]);
        if (target) QueueJavaTask(static_cast<FakeObject *>(target->refs["looper"]),
                                 static_cast<FakeObject *>(target->refs["callback"]), "handleMessage", obj);
        return nullptr;
    }
    if (cls == "android/view/Choreographer" && name == "getInstance") {
        auto *choreographer = NewObject(cls);
        choreographer->refs["looper"] = g_threadLooper;
        return choreographer;
    }
    if (cls == "java/lang/Long" && name == "longValue") return (void *)(intptr_t)obj->ints["value"];
    if (cls == "java/lang/Boolean" && name == "booleanValue") return (void *)(intptr_t)obj->ints["value"];
    if ((cls == "java/lang/Long" || cls == "java/lang/Boolean" || cls == "java/lang/Integer") && name == "valueOf") {
        auto *boxed = NewObject(cls); boxed->ints["value"] = (intptr_t)arg0; return boxed;
    }

    if (cls == "android/content/Context") {
        if (name == "getAssets") return NewObject("android/content/res/AssetManager");
        if (name == "getResources") return NewObject("android/content/res/Resources");
        if (name == "getApplicationContext") return obj;
        if (name == "getClassLoader") return NewObject("java/lang/ClassLoader");
        if (name == "getPackageName") {
            FakeObject *s = NewObject("java/lang/String");
            s->strs["value"] = g_packageName;
            return s;
        }
        if (name == "getFilesDir" || name == "getCacheDir" || name == "getExternalFilesDir" ||
            name == "getExternalCacheDir" || name == "getDir") {
            FakeObject *file = NewObject("java/io/File");
            file->strs["path"] = name == "getCacheDir"
                                     ? "/data/storage/el2/base/cache"
                                     : "/data/storage/el2/base/files";
            return file;
        }
        if (name == "getSystemService") return NewObject("android/content/Context$SystemService");
        if (name == "getApplicationInfo") {
            return NewObject("android/content/pm/ApplicationInfo");
        }
        if (name == "getPackageManager") {
            return NewObject("android/content/pm/PackageManager");
        }
        if (name == "getPackageCodePath" || name == "getPackageResourcePath") {
            FakeObject *s = NewObject("java/lang/String");
            s->strs["value"] = "/data/storage/el2/base/files/rea-original-phigros.apk";
            return s;
        }
        if (name == "getObbDir") {
            FakeObject *file = NewObject("java/io/File");
            file->strs["path"] = "/data/storage/el2/base/files";
            return file;
        }
        if (name == "getContentResolver") return NewObject("android/content/ContentResolver");
        if (name == "getMainLooper") return NewObject("android/os/Looper");
        if (name == "getClass" && obj) {auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]=obj->cls;return clazz;}
    }

    if (cls == "android/content/res/AssetManager") {
        if (name == "openFd") {
            std::string path = ArgToString(arg0);
            FakeObject *afd = OpenAssetFd(path.c_str());
            if (!afd) {
                /* Returning null makes the engine fall back or raise; keep the log honest. */
                return nullptr;
            }
            return afd;
        }
        if (name == "open") {
            FakeObject *afd = OpenAssetFd(ArgToString(arg0).c_str());
            if (!afd) return nullptr;
            FakeObject *stream = NewObject("java/io/FileInputStream");
            stream->ints["descriptor"] = afd->fd;
            stream->ints["offset"] = afd->startOffset;
            stream->ints["length"] = afd->length;
            return stream;
        }
        if (name == "list") return NewObject("[Ljava/lang/String;");
        if (name == "close") return nullptr;
    }

    if (cls == "java/io/FileInputStream" || cls == "java/io/BufferedInputStream") {
        int fd = (int)obj->ints["descriptor"];
        long long offset = obj->ints.count("offset") ? obj->ints["offset"] : 0;
        long long total = obj->ints["length"];
        long long position = obj->ints.count("pos") ? obj->ints["pos"] : 0;

        if (name == "available") {
            long long remaining = total - position;
            return (void *)(intptr_t)(remaining > 0 ? (long)remaining : 0);
        }
        if (name == "close") return nullptr;
        if (name == "skip") {
            long long skip = (long long)(intptr_t)arg0;
            long long next = position + skip;
            if (next > total) next = total;
            obj->ints["pos"] = next;
            return (void *)(intptr_t)(next - position);
        }
        if (name == "read") {
            FakeObject *buffer = (FakeObject *)arg0;
            if (!buffer) {
                /* read(): a single byte. */
                unsigned char byte = 0;
                if (position >= total) return (void *)(intptr_t)-1;
                if (ReaReadAt(fd, &byte, 1, offset + position) != 1) return (void *)(intptr_t)-1;
                obj->ints["pos"] = position + 1;
                return (void *)(intptr_t)byte;
            }
            size_t start = 0;
            size_t want = buffer->strs["bytes"].size();
            long long remaining = total - position;
            if ((long long)want > remaining) want = (size_t)(remaining > 0 ? remaining : 0);
            if (want == 0) return (void *)(intptr_t)-1;
            long got = ReaReadAt(fd, &buffer->strs["bytes"][0] + start, (unsigned long)want,
                                 offset + position);
            if (got <= 0) return (void *)(intptr_t)-1;
            obj->ints["pos"] = position + got;
            return (void *)(intptr_t)got;
        }
    }

    if (cls == "android/content/res/AssetFileDescriptor") {
        if (name == "getFileDescriptor") {
            FakeObject *fdesc = NewObject("java/io/FileDescriptor");
            fdesc->ints["descriptor"] = obj->fd;
            return fdesc;
        }
        if (name == "getStartOffset") return (void *)(intptr_t)obj->startOffset;
        if (name == "getLength") return (void *)(intptr_t)obj->length;
        if (name == "getDeclaredLength") return (void *)(intptr_t)obj->length;
        if (name == "close") {
            if (obj->ownsFd && obj->fd >= 0) {
                close(obj->fd);
                obj->ownsFd = false;
            }
            return nullptr;
        }
    }

    if (cls == "android/content/res/Resources") {
        if (name == "getAssets") return NewObject("android/content/res/AssetManager");
        if (name == "getDisplayMetrics") return NewObject("android/util/DisplayMetrics");
        if (name == "getConfiguration") return NewObject("android/content/res/Configuration");
        if (name == "getIdentifier") return (void *)(intptr_t)0;
    }

    if (cls == "java/io/File") {
        if (name == "getAbsolutePath" || name == "getPath" || name == "toString") {
            FakeObject *s = NewObject("java/lang/String");
            auto it = obj->strs.find("path");
            s->strs["value"] = it != obj->strs.end() ? it->second : "/data/storage/el2/base/files";
            return s;
        }
        if (name == "exists") return (void *)(intptr_t)1;
        if (name == "mkdirs" || name == "mkdir") return (void *)(intptr_t)1;
    }

    if (cls == "android/view/Surface") {
        if (name == "isValid") return (void *)(intptr_t)1;
        if (name == "getWidth" || name == "getHeight") {
            int32_t width = 1920;
            int32_t height = 1080;
            if (ReaShim_GetNativeWindowSize(&width, &height)) {
                return (void *)(intptr_t)(name == "getWidth" ? width : height);
            }
            return (void *)(intptr_t)(name == "getWidth" ? width : height);
        }
        if (name == "toString") {
            FakeObject *s = NewObject("java/lang/String");
            s->strs["value"] = "Surface(ohos)";
            return s;
        }
    }

    if (cls == "android/util/DisplayMetrics") {
        if (name == "toString") {
            FakeObject *s = NewObject("java/lang/String");
            s->strs["value"] = "DisplayMetrics";
            return s;
        }
    }

    if (cls == "android/app/Activity") {
        if (name == "getWindowManager" || name == "getWindow") {
            return NewObject("android/view/WindowManager");
        }
        if (name == "runOnUiThread") return nullptr;
        if (name == "getIntent") return NewObject("android/content/Intent");
        if (name == "getApplication") return NewObject("android/app/Application");
    }

    if (cls == "java/lang/Class") {
        if (name == "getClassLoader") return NewObject("java/lang/ClassLoader");
        if (name == "getName" || name == "toString") {
            FakeObject *s = NewObject("java/lang/String");
            auto it = obj->strs.find("name");
            s->strs["value"] = it != obj->strs.end() ? it->second : "";
            std::replace(s->strs["value"].begin(),s->strs["value"].end(),'/','.');
            return s;
        }
        if (name == "getMethod") return NewObject("java/lang/reflect/Method");
    }

    if (cls == "java/lang/ClassLoader") {
        if (name == "loadClass" || name == "findClass") {auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]=ArgToString(arg0);std::replace(clazz->strs["name"].begin(),clazz->strs["name"].end(),'.','/');return clazz;}
        if (name == "getParent") return nullptr;
    }

    if (cls == "java/io/FileInputStream") {
        if (name == "getFD") {
            FakeObject *fdesc = NewObject("java/io/FileDescriptor");
            fdesc->ints["descriptor"] = obj->ints["descriptor"];
            return fdesc;
        }
        if (name == "read") return (void *)(intptr_t)0;
        if (name == "close") return nullptr;
    }

    Emit("UNHANDLED_CALL %s.%s %s", cls.c_str(), name.c_str(), m->signature.c_str());
    auto close = m->signature.find(')');
    if (close != std::string::npos && close+1 < m->signature.size() &&
        strchr("VZBCSIJFD", m->signature[close+1])) return nullptr;
    return NewObject(cls + "$result");
}

} // namespace

extern "C" void *ReaJniDispatch(int index, void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h)
{
    NoteSlot(index);

    const char *name = (index >= 0 && index < 240) ? g_jni_names[index] : "";
    (void)e; (void)f; (void)g; (void)h;

    if (strcmp(name, "FindClass") == 0) {
        const char *className = (const char *)b;
        Emit("FindClass(\"%s\")", className ? className : "");
        FakeObject *clazz = NewObject("java/lang/Class");
        clazz->strs["name"] = className ? className : "";
        return clazz;
    }

    if (strcmp(name, "GetObjectClass") == 0) {
        FakeObject *obj = (FakeObject *)b;
        FakeObject *clazz = NewObject("java/lang/Class");
        clazz->strs["name"] = obj ? obj->cls : "";
        return clazz;
    }

    if (strcmp(name, "FromReflectedMethod") == 0 || strcmp(name,"FromReflectedField")==0) {
        auto *method = static_cast<FakeObject *>(b);
        return method ? method->refs[strcmp(name,"FromReflectedField")==0?"field":"method"] : nullptr;
    }
    if (strcmp(name, "IsInstanceOf") == 0) {
        auto *object = static_cast<FakeObject *>(b);
        auto *clazz = static_cast<FakeObject *>(c);
        if (!clazz) return nullptr;
        if (!object) return (void *)1; // JNI null is assignable to every reference type
        const auto &wanted = clazz->strs["name"];
        if (wanted == "java/lang/Object" || wanted == object->cls) return (void *)1;
        auto *interfaces = static_cast<FakeObject *>(object->refs["interfaces"]);
        if (interfaces) for (const auto &item : interfaces->refs) {
            auto *entry = static_cast<FakeObject *>(item.second);
            if (entry && entry->strs["name"] == wanted) return (void *)1;
        }
        return nullptr;
    }

    if (strcmp(name, "GetMethodID") == 0 || strcmp(name, "GetStaticMethodID") == 0) {
        FakeObject *clazz = (FakeObject *)b;
        std::string cls = clazz ? clazz->strs["name"] : "";
        const char *methodName = c ? (const char *)c : "";
        if (cls.find("Looper") != std::string::npos || cls.find("Handler") != std::string::npos ||
            cls.find("Thread") != std::string::npos || strstr(methodName, "Proxy"))
            Emit("JNI_METHOD %s.%s %s", cls.c_str(), methodName, d ? (const char *)d : "");
        return NewMethod(cls, (const char *)c ? (const char *)c : "", (const char *)d ? (const char *)d : "",
                         strcmp(name, "GetStaticMethodID") == 0, false);
    }

    if (strcmp(name, "GetFieldID") == 0 || strcmp(name, "GetStaticFieldID") == 0) {
        FakeObject *clazz = (FakeObject *)b;
        std::string cls = clazz ? clazz->strs["name"] : "";
        return NewMethod(cls, (const char *)c ? (const char *)c : "", (const char *)d ? (const char *)d : "",
                         strcmp(name, "GetStaticFieldID") == 0, true);
    }

    if (strcmp(name, "AllocObject") == 0 || strcmp(name, "NewObject") == 0 ||
        strcmp(name, "NewObjectV") == 0 || strcmp(name, "NewObjectA") == 0) {
        auto *clazz = static_cast<FakeObject *>(b);
        auto *object = NewObject(clazz ? clazz->strs["name"] : "java/lang/Object");
        if (strcmp(name, "AllocObject") != 0) {
            void *first = d, *second = e;
            if ((strcmp(name, "NewObjectV") == 0 || strcmp(name, "NewObjectA") == 0) &&
                !FrameworkArguments(static_cast<MethodRef *>(c), d, strcmp(name, "NewObjectA") == 0, &first, &second))
                return nullptr;
            DispatchObjectCall(object, static_cast<MethodRef *>(c), first, second);
        }
        return object;
    }

    if (strcmp(name, "NewObjectArray") == 0) {
        int64_t length = (intptr_t)b;
        if (length < 0 || length > (1 << 20)) return nullptr;
        auto *array = NewObject("[Ljava/lang/Object;");
        array->ints["length"] = length;
        for (int64_t i=0;i<length;++i) array->refs["["+std::to_string(i)+"]"] = d;
        return array;
    }
    if (strcmp(name, "GetObjectArrayElement") == 0 || strcmp(name, "SetObjectArrayElement") == 0) {
        auto *array = static_cast<FakeObject *>(b);
        int64_t position = (intptr_t)c;
        if (!array || position < 0 || position >= array->ints["length"]) return nullptr;
        std::string key = "["+std::to_string(position)+"]";
        if (strcmp(name, "SetObjectArrayElement") == 0) { array->refs[key] = d; return nullptr; }
        return array->refs[key];
    }

    if (strcmp(name, "CallObjectMethod") == 0 || strcmp(name, "CallStaticObjectMethod") == 0) {
        /* Unity's C++ shim passes the first argument in the register slot right after the method. */
        return DispatchObjectCall((FakeObject *)b, (MethodRef *)c, d, e,f,g);
    }
    if (strcmp(name, "CallObjectMethodV") == 0 || strcmp(name, "CallStaticObjectMethodV") == 0 ||
        strcmp(name, "CallIntMethodV") == 0 || strcmp(name, "CallStaticIntMethodV") == 0 ||
        strcmp(name, "CallLongMethodV") == 0 || strcmp(name, "CallStaticLongMethodV") == 0 ||
        strcmp(name, "CallBooleanMethodV") == 0 || strcmp(name, "CallStaticBooleanMethodV") == 0 ||
        strcmp(name, "CallVoidMethodV") == 0 || strcmp(name, "CallStaticVoidMethodV") == 0 ||
        strcmp(name, "CallObjectMethodA") == 0 || strcmp(name, "CallStaticObjectMethodA") == 0 ||
        strcmp(name, "CallIntMethodA") == 0 || strcmp(name, "CallStaticIntMethodA") == 0 ||
        strcmp(name, "CallLongMethodA") == 0 || strcmp(name, "CallStaticLongMethodA") == 0 ||
        strcmp(name, "CallBooleanMethodA") == 0 || strcmp(name, "CallStaticBooleanMethodA") == 0 ||
        strcmp(name, "CallVoidMethodA") == 0 || strcmp(name, "CallStaticVoidMethodA") == 0) {
        auto *platformMethod=static_cast<MethodRef *>(c);
        bool array = name[strlen(name)-1]=='A';
        if(platformMethod && ((platformMethod->cls=="com/unity3d/player/UnityPlayer" &&
           (platformMethod->name=="showSoftInput"||platformMethod->name=="hideSoftInput"||platformMethod->name=="setSoftInputStr"||
            platformMethod->name=="setSelection"||platformMethod->name=="setCharacterLimit"))||platformMethod->cls=="com/pingak9/nativepopup/Bridge")){
            rea_jni_args::Arguments args;std::string error;bool valid=false;
            if(array){rea_jni_args::Descriptor desc;if(rea_jni_args::Parse(platformMethod->signature.c_str(),desc,error)){
                std::vector<rea_jni_args::JValue> values(desc.parameters.size());
                if(values.empty()||d){if(!values.empty())memcpy(values.data(),d,values.size()*8);valid=rea_jni_args::DecodeA(platformMethod->signature.c_str(),values.data(),args,error);}
            }}else valid=rea_jni_args::DecodeV(platformMethod->signature.c_str(),static_cast<va_list *>(d),args,error);
            if(valid&&(HandleKeyboardCall(platformMethod,args)||HandleNativeDialogCall(platformMethod,args)))return nullptr;
            Emit("IME_ARGUMENT_ERROR method=%s",platformMethod->name.c_str());return nullptr;
        }
        void *first=nullptr, *second=nullptr,*third=nullptr,*fourth=nullptr;
        if (!FrameworkArguments(static_cast<MethodRef *>(c), d, array, &first, &second,&third,&fourth)) return nullptr;
        void *result=DispatchObjectCall(static_cast<FakeObject *>(b), static_cast<MethodRef *>(c), first, second,third,fourth);
        if (strstr(name, "BooleanMethod")) return reinterpret_cast<void *>(static_cast<intptr_t>(result ? 1 : 0));
        if (strstr(name, "VoidMethod")) return nullptr;
        return result;
    }

    if (strcmp(name, "CallIntMethod") == 0 || strcmp(name, "CallStaticIntMethod") == 0 ||
        strcmp(name, "CallLongMethod") == 0 || strcmp(name, "CallStaticLongMethod") == 0) {
        void *result = DispatchObjectCall((FakeObject *)b, (MethodRef *)c, d, e);
        return result;
    }
    if (strcmp(name, "CallBooleanMethod") == 0 || strcmp(name, "CallStaticBooleanMethod") == 0) {
        void *result = DispatchObjectCall((FakeObject *)b, (MethodRef *)c, d, e);
        return result ? (void *)(intptr_t)1 : (void *)(intptr_t)0;
    }
    if (strcmp(name, "CallVoidMethod") == 0 || strcmp(name, "CallStaticVoidMethod") == 0) {
        DispatchObjectCall((FakeObject *)b, (MethodRef *)c, d, e);
        return nullptr;
    }

    if (strcmp(name, "GetIntField") == 0 || strcmp(name, "GetStaticIntField") == 0) {
        FakeObject *obj = (FakeObject *)b;
        MethodRef *field = (MethodRef *)c;
        if(field&&field->cls=="android/media/MediaCodecInfo$CodecCapabilities"){
            if(field->name=="COLOR_FormatYUV420Planar")return (void *)(intptr_t)19;
            if(field->name=="COLOR_FormatYUV420SemiPlanar")return (void *)(intptr_t)21;
            if(field->name=="COLOR_FormatYUV420Flexible")return (void *)(intptr_t)0x7f420888;
            if(field->name=="COLOR_FormatSurface")return (void *)(intptr_t)0x7f000789;
        }
        /* android.os.Build$VERSION.sdkInt decides which platform paths the engine takes. */
        if (field && (field->name == "sdkInt" || field->name == "SDK_INT")) {
            return (void *)(intptr_t)33;
        }
        if (obj && field) {
            auto it = obj->ints.find(field->name);
            if (it != obj->ints.end()) {
                if (field->name == "descriptor") {
                    Emit("GetIntField(FileDescriptor.descriptor) -> fd=%lld", (long long)it->second);
                }
                return (void *)(intptr_t)it->second;
            }
            Emit("GetIntField(%s.%s) -> 0 (absent)", obj->cls.c_str(), field->name.c_str());
        }
        return (void *)(intptr_t)0;
    }
    if (strcmp(name, "GetLongField") == 0 || strcmp(name, "GetStaticLongField") == 0) {
        FakeObject *obj = (FakeObject *)b;
        MethodRef *field = (MethodRef *)c;
        if (obj && field) {
            auto it = obj->ints.find(field->name);
            if (it != obj->ints.end()) return (void *)(intptr_t)it->second;
        }
        return (void *)(intptr_t)0;
    }
    if (strcmp(name, "GetObjectField") == 0 || strcmp(name, "GetStaticObjectField") == 0) {
        FakeObject *obj = (FakeObject *)b;
        MethodRef *field = (MethodRef *)c;
        if (obj && field) {
            auto it = obj->refs.find(field->name);
            if (it != obj->refs.end()) return it->second;
            if(field->cls=="com/unity3d/player/UnityPlayer"&&field->name=="currentActivity")return g_currentActivity;
            if(field->cls=="android/content/res/Configuration"&&field->name=="locale")return NewObject("java/util/Locale");

            /*
             * Values the engine reads from the platform. android.os.Build strings are compared against
             * known device names (an unset one made a strcasecmp fault inside musl), and the
             * ApplicationInfo paths tell the engine where its libraries and data live.
             */
            /* Context.AUDIO_SERVICE and friends hold the name passed to getSystemService. */
            if (field->name.size() > 8 &&
                field->name.compare(field->name.size() - 8, 8, "_SERVICE") == 0) {
                std::string service = field->name.substr(0, field->name.size() - 8);
                for (char &character : service) character = (char)tolower((unsigned char)character);
                return MakeStringObject(service.c_str());
            }

            const char *value = nullptr;
            if (field->name == "MANUFACTURER" || field->name == "BRAND") value = "HUAWEI";
            else if (field->name == "MODEL" || field->name == "DEVICE" || field->name == "PRODUCT")
                value = "PCE-W40";
            else if (field->name == "BOARD" || field->name == "HARDWARE") value = "kirin";
            else if (field->name == "ID") value = "SP1A";
            else if (field->name == "TAGS") value = "release-keys";
            else if (field->name == "TYPE") value = "user";
            else if (field->name == "FINGERPRINT")
                value = "HUAWEI/PCE-W40/PCE-W40:12/SP1A/1:user/release-keys";
            else if (field->name == "nativeLibraryDir")
                value = "/data/storage/el1/bundle/libs/arm64";
            else if (field->name == "nativeLibraryRootDir") value = "/data/storage/el1/bundle/libs";
            else if (field->name == "sourceDir" || field->name == "publicSourceDir")
                value = "/data/storage/el2/base/files/rea-original-phigros.apk";
            else if (field->name == "dataDir") value = "/data/storage/el2/base/files";
            else if (field->name == "packageName") value = g_packageName;
            if (value) {
                FakeObject *s = NewObject("java/lang/String");
                s->strs["value"] = value;
                return s;
            }
        }
        return nullptr;
    }
    if (strcmp(name, "SetIntField") == 0 || strcmp(name, "SetLongField") == 0) {
        FakeObject *obj = (FakeObject *)b;
        MethodRef *field = (MethodRef *)c;
        if (obj && field) obj->ints[field->name] = (int64_t)(intptr_t)d;
        return nullptr;
    }

    if(strcmp(name,"GetDirectBufferAddress")==0)return b?static_cast<FakeObject *>(b)->refs["direct"]:nullptr;
    if(strcmp(name,"GetDirectBufferCapacity")==0)return b?(void *)(intptr_t)static_cast<FakeObject *>(b)->ints["capacity"]:(void *)-1;
    if(strcmp(name,"NewDirectByteBuffer")==0){auto *object=NewObject("java/nio/DirectByteBuffer");object->refs["direct"]=b;object->ints["capacity"]=(intptr_t)c;return object;}
    if (strcmp(name, "NewStringUTF") == 0 || strcmp(name, "NewString") == 0) {
        FakeObject *s = NewObject("java/lang/String");
        if (strcmp(name, "NewString") == 0) {
            size_t length = (uintptr_t)c;
            if (length > (1<<24) || (!b && length)) return nullptr;
            if (length) s->utf16.assign(static_cast<const char16_t *>(b), length);
            try { s->strs["value"] = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(s->utf16); }
            catch (...) { return nullptr; }
        } else s->strs["value"] = b ? (const char *)b : "";
        return s;
    }
    if(strstr(name,"Array")){
    const char *primitiveNames[]={"Boolean","Char","Short","Int","Long","Float","Double"};
    const char primitiveTypes[]={'Z','C','S','I','J','F','D'};const size_t primitiveSizes[]={1,2,2,4,8,4,8};
    for(size_t k=0;k<7;++k){
        std::string type=primitiveNames[k];size_t width=primitiveSizes[k];
        if(std::string(name)=="New"+type+"Array"){
            intptr_t length=(intptr_t)b;if(length<0||length>int64_t(64*1024*1024/width))return nullptr;
            auto *array=NewObject(std::string("[")+primitiveTypes[k]);array->ints["length"]=length;
            array->strs["bytes"].resize(length*width);return array;
        }
        bool get=std::string(name)=="Get"+type+"ArrayElements",region=std::string(name)=="Get"+type+"ArrayRegion",set=std::string(name)=="Set"+type+"ArrayRegion";
        if(get||region||set){
            auto *array=static_cast<FakeObject *>(b);if(!array)return nullptr;auto &bytes=array->strs["bytes"];
            if(get){if(c)*static_cast<unsigned char *>(c)=0;return bytes.empty()?nullptr:&bytes[0];}
            intptr_t start=(intptr_t)c,length=(intptr_t)d;if(start<0||length<0||size_t(start)>bytes.size()/width||size_t(length)>bytes.size()/width-size_t(start)||!e)return nullptr;
            if(set)memcpy(&bytes[size_t(start)*width],e,size_t(length)*width);else memcpy(e,bytes.data()+size_t(start)*width,size_t(length)*width);return nullptr;
        }
        if(std::string(name)=="Release"+type+"ArrayElements")return nullptr;
    }
    if(!strcmp(name,"GetPrimitiveArrayCritical")){
        auto *array=static_cast<FakeObject *>(b);if(c)*static_cast<unsigned char *>(c)=0;
        return array&&!array->strs["bytes"].empty()?&array->strs["bytes"][0]:nullptr;
    }
    if(!strcmp(name,"ReleasePrimitiveArrayCritical"))return nullptr;
    }
    /* Byte arrays back everything the engine reads from an InputStream. */
    if (strcmp(name, "NewByteArray") == 0) {
        FakeObject *array = NewObject("[B");
        int length = (int)(intptr_t)b;
        if (length < 0) length = 0;
        if (length > (1 << 20)) length = 1 << 20;
        array->strs["bytes"] = std::string((size_t)length, '\0');
        return array;
    }
    if (strcmp(name, "GetArrayLength") == 0) {
        FakeObject *array = (FakeObject *)b;
        if (!array) return nullptr;
        auto length = array->ints.find("length");
        return (void *)(intptr_t)(length == array->ints.end() ? array->strs["bytes"].size() : length->second);
    }
    if (strcmp(name, "GetByteArrayElements") == 0 || strcmp(name, "GetByteArrayRegion") == 0) {
        FakeObject *array = (FakeObject *)b;
        if (!array) return nullptr;
        if (array->strs["bytes"].empty()) array->strs["bytes"].resize(1);
        if (strcmp(name, "GetByteArrayRegion") == 0) {
            /* GetByteArrayRegion(src, start, len, dest) */
            size_t start = (size_t)(uintptr_t)c;
            size_t length = (size_t)(uintptr_t)d;
            void *destination = e;
            if (destination && start + length <= array->strs["bytes"].size()) {
                memcpy(destination, array->strs["bytes"].data() + start, length);
            }
            return nullptr;
        }
        return (void *)array->strs["bytes"].data();
    }
    if (strcmp(name, "ReleaseByteArrayElements") == 0 ||
        strcmp(name, "SetByteArrayRegion") == 0) {
        if (strcmp(name, "SetByteArrayRegion") == 0) {
            auto *array = static_cast<FakeObject *>(b);
            size_t start = (uintptr_t)c, length = (uintptr_t)d;
            if (array && start <= array->strs["bytes"].size() && length <= array->strs["bytes"].size()-start && e)
                memcpy(&array->strs["bytes"][start], e, length);
        }
        return nullptr;
    }
    if (strcmp(name, "GetStringUTFChars") == 0 || strcmp(name, "GetStringChars") == 0) {
        FakeObject *s = (FakeObject *)b;
        if (strcmp(name, "GetStringChars") == 0 && s) {
            try { s->utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(s->strs["value"]); }
            catch (...) { return nullptr; }
            return (void *)s->utf16.c_str();
        }
        return (void *)(s ? s->strs["value"].c_str() : "");
    }
    if (strcmp(name, "ReleaseStringUTFChars") == 0 || strcmp(name, "ReleaseStringChars") == 0) {
        return nullptr;
    }
    if (strcmp(name, "GetStringUTFLength") == 0 || strcmp(name, "GetStringLength") == 0) {
        FakeObject *s = (FakeObject *)b;
        if (strcmp(name, "GetStringLength") == 0 && s) {
            try { return (void *)(intptr_t)std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(s->strs["value"]).size(); }
            catch (...) { return nullptr; }
        }
        return (void *)(intptr_t)(s ? s->strs["value"].size() : 0);
    }

    if (strcmp(name, "RegisterNatives") == 0) {
        FakeObject *clazz = (FakeObject *)b;
        int count = (int)(intptr_t)d;
        Emit("RegisterNatives(%s, count=%d)", clazz ? clazz->strs["name"].c_str() : "?", count);
        CaptureRegistrations(clazz, c, count);
        return (void *)0;
    }

    // Fake Java objects are registry-owned for the process lifetime. References alias those
    // stable objects, including the weak/global/local sequence used by AndroidJavaProxy.
    if (strcmp(name, "NewGlobalRef") == 0 || strcmp(name,"NewLocalRef")==0 || strcmp(name,"NewWeakGlobalRef")==0) return b;
    if (strcmp(name, "DeleteGlobalRef") == 0 || strcmp(name, "DeleteLocalRef") == 0 || strcmp(name,"DeleteWeakGlobalRef")==0) return nullptr;
    if (strcmp(name, "IsSameObject") == 0) return (void *)(intptr_t)(b == c ? 1 : 0);
    if (strcmp(name, "PushLocalFrame") == 0) return (void *)(intptr_t)0;
    if (strcmp(name, "PopLocalFrame") == 0) return b;
    if (strcmp(name, "ExceptionCheck") == 0) return (void *)(intptr_t)(g_pendingException?1:0);
    if (strcmp(name, "ExceptionOccurred") == 0) return g_pendingException;
    if (strcmp(name, "ExceptionClear") == 0){g_pendingException=nullptr;return nullptr;}
    if (strcmp(name, "ExceptionDescribe") == 0){if(g_pendingException)Emit("JAVA_EXCEPTION %s %s",g_pendingException->cls.c_str(),g_pendingException->strs["message"].c_str());return nullptr;}
    if (strcmp(name,"Throw")==0){g_pendingException=static_cast<FakeObject *>(b);return nullptr;}
    if (strcmp(name,"ThrowNew")==0){auto *clazz=static_cast<FakeObject *>(b);g_pendingException=NewObject(clazz?clazz->strs["name"]:"java/lang/Throwable");g_pendingException->strs["message"]=c?static_cast<const char *>(c):"";return nullptr;}
    if (strcmp(name, "GetVersion") == 0) return (void *)(intptr_t)0x00010006;
    if (strcmp(name, "GetJavaVM") == 0) {
        if (b) *(void **)b = ReaFakeVmHandle();
        return (void *)(intptr_t)0;
    }

    return nullptr;
}

/* ---- fault reporting --------------------------------------------------------------------- */

/* Set by the calibration crash so the handler can locate pc by address range. */
unsigned long g_calibStart = 0;

namespace {

void FaultHandler(int sig, siginfo_t *info, void *uctx)
{
    char buffer[768];
    unsigned long *words = (unsigned long *)uctx;

    /*
     * Locate the faulting stack pointer: this handler runs on the same stack as the faulting code,
     * so its own frame address is close to sp. Once sp's index is known, sigcontext gives the rest.
     */
    /*
     * mcontext starts at word 22 on this platform, measured with a deliberate crash whose frame
     * address was known (see tools/patch_calibrate.py). sigcontext layout:
     *   fault_address, regs[0..30], sp, pc, pstate
     */
    /* During calibration, find pc by locating a word inside the crashing function itself. */
    if (g_calibStart) {
        for (int i = 0; i < 70; i++) {
            if (words[i] >= g_calibStart && words[i] < g_calibStart + 0x400) {
                char cal[128];
                snprintf(cal, sizeof(cal), "CALIB pc at word %d (fn+0x%lx)", i, words[i] - g_calibStart);
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", cal);
                break;
            }
        }
    }

    // Use the ABI we compile against; guessed word indices reported a saved
    // scratch register as PC and incorrectly identified the faulting instruction.
    const ucontext_t *context = static_cast<const ucontext_t *>(uctx);
    unsigned long faultAddress = context->uc_mcontext.fault_address;
    unsigned long linkRegister = context->uc_mcontext.regs[30];
    unsigned long stackPointer = context->uc_mcontext.sp;
    unsigned long programCounter = context->uc_mcontext.pc;
    OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG,
        "FAULT_CONTEXT sdk_mcontext_offset=%{public}zu pc=%{public}lx sp=%{public}lx lr=%{public}lx x0=%{public}lx x1=%{public}lx x19=%{public}lx x20=%{public}lx x21=%{public}lx x22=%{public}lx",
        offsetof(ucontext_t, uc_mcontext), programCounter, stackPointer, linkRegister,
        context->uc_mcontext.regs[0], context->uc_mcontext.regs[1],
        context->uc_mcontext.regs[19], context->uc_mcontext.regs[20],
        context->uc_mcontext.regs[21], context->uc_mcontext.regs[22]);
    ReaLogEglState("engine fault");
    // Only inspect memory within the already allocated registry placeholders.
    // No engine state is written by this diagnostic.
    if (g_unityBase) {
        const auto *table = reinterpret_cast<const uintptr_t *>(g_unityBase + 0x1379148UL);
        uintptr_t map = context->uc_mcontext.regs[19];
        for (int i = 0; i < 32; ++i) {
            uintptr_t owner = table[i];
            if (owner && map >= owner && map - owner <= 0xFC0 && (map & 7) == 0) {
                const auto *data = reinterpret_cast<const uintptr_t *>(map);
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG,
                    "FAULT_MAP registry=%{public}d owner=%{public}lx offset=%{public}lx words=%{public}lx,%{public}lx,%{public}lx,%{public}lx",
                    i, owner, map-owner, data[0], data[1], data[2], data[3]);
            }
        }
    }

    /* Attribute the faulting address to a loaded library so it maps to a known offset. */
    Dl_info where;
    memset(&where, 0, sizeof(where));
    const char *module = "?";
    unsigned long moduleOffset = 0;
    if (programCounter && dladdr((void *)programCounter, &where) && where.dli_fname) {
        module = where.dli_fname;
        moduleOffset = programCounter - (unsigned long)(uintptr_t)where.dli_fbase;
    }

    Dl_info lrWhere;
    memset(&lrWhere, 0, sizeof(lrWhere));
    const char *lrModule = "?";
    unsigned long lrOffset = 0;
    if (linkRegister && dladdr((void *)linkRegister, &lrWhere) && lrWhere.dli_fname) {
        lrModule = lrWhere.dli_fname;
        lrOffset = linkRegister - (unsigned long)(uintptr_t)lrWhere.dli_fbase;
    }

    int written = snprintf(buffer, sizeof(buffer),
                           "FAULT signal=%d addr=%p pc=0x%lx (%s+0x%lx) lr=0x%lx (%s+0x%lx)",
                           sig, info ? info->si_addr : nullptr, programCounter,
                           module, moduleOffset, linkRegister, lrModule, lrOffset);
    if (written > 0) {
        OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", buffer);

        /* Unwind from the signal frame; this reaches the faulting call chain. */
        {
            void *frames[40];
            int frameCount = backtrace(frames, 40);
            char **symbols = backtrace_symbols(frames, frameCount);
            for (int i = 0; i < frameCount; i++) {
                const char *text = symbols ? symbols[i] : "(no symbols)";
                char frameLine[320];
                int frameLen = snprintf(frameLine, sizeof(frameLine), "FAULT_BT #%02d %p %s",
                                        i, frames[i], text ? text : "");
                if (frameLen > 0) {
                    OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", frameLine);
                }
                int fd2 = open("/data/storage/el2/base/files/rea_fault.txt",
                               O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd2 >= 0) {
                    ssize_t ignored2 = write(fd2, frameLine, (size_t)frameLen);
                    ignored2 = write(fd2, "\n", 1);
                    (void)ignored2;
                    close(fd2);
                }
            }
            if (symbols) free(symbols);
        }


        /* Extended dump: the calibration run needs words past the first window. */
        {
            char wide[1024];
            int wideLen = snprintf(wide, sizeof(wide),
                                   "FAULT_WIDE %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx "
                                   "%lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx",
                                   words[40], words[41], words[42], words[43], words[44], words[45],
                                   words[46], words[47], words[48], words[49], words[50], words[51],
                                   words[52], words[53], words[54], words[55], words[56], words[57],
                                   words[58], words[59], words[60], words[61], words[62], words[63],
                                   words[64], words[65], words[66], words[67]);
            if (wideLen > 0) {
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", wide);
            }
        }
        /* Raw ucontext words, so a wrong offset assumption is visible instead of misleading. */
        char rawBuffer[640];
        int rawLength = snprintf(rawBuffer, sizeof(rawBuffer),
                                 "FAULT_RAW %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx "
                                 "%lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx",
                                 words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7],
                                 words[8], words[9], words[10], words[11], words[12], words[13], words[14],
                                 words[15], words[16], words[17], words[18], words[19], words[20], words[21],
                                 words[22], words[23], words[24], words[25], words[26], words[27], words[28],
                                 words[29], words[30], words[31]);
        char rawBuffer2[640];
        int rawLength2 = snprintf(rawBuffer2, sizeof(rawBuffer2),
                                  "FAULT_RAW2 %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx",
                                  words[32], words[33], words[34], words[35], words[36], words[37],
                                  words[38], words[39], words[40], words[41], words[42], words[43],
                                  words[44], words[45], words[46], words[47]);

        /* Walk the faulting stack for return addresses: the readable substitute for a backtrace. */
        {
            unsigned long scanBase = stackPointer;
            if (scanBase) {
                ptrdiff_t step = 1;
                int printed = 0;
                for (unsigned long addr = scanBase; addr < scanBase + 0x8000 && printed < 18; addr += 8) {
                    unsigned long value;
                    memcpy(&value, (void *)addr, sizeof(value));
                    if (!value) continue;
                    Dl_info frameInfo;
                    memset(&frameInfo, 0, sizeof(frameInfo));
                    if (!dladdr((void *)value, &frameInfo) || !frameInfo.dli_fname) continue;
                    if (!strstr(frameInfo.dli_fname, "libunity") && !strstr(frameInfo.dli_fname, "libil2cpp")) continue;
                    unsigned long offset = value - (unsigned long)(uintptr_t)frameInfo.dli_fbase;
                    char frameLine[256];
                    int frameLen = snprintf(frameLine, sizeof(frameLine), "FAULT_FRAME +0x%lx %s",
                                            offset, frameInfo.dli_fname);
                    if (frameLen > 0) {
                        OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", frameLine);
                    }
                    printed++;
                }
                (void)step;
                if (printed == 0) {
                    OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG,
                                 "%{public}s", "FAULT_FRAME none found in the scanned window");
                }
            }
        }
        if (rawLength > 0) OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", rawBuffer);
        if (rawLength2 > 0) OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", rawBuffer2);
        /* Attribute every plausible register word to a module, so the layout stops mattering. */
        for (int slot = 5; slot < 48; slot++) {
            unsigned long candidate = words[slot];
            if (!candidate) continue;
            Dl_info candInfo;
            memset(&candInfo, 0, sizeof(candInfo));
            if (!dladdr((void *)candidate, &candInfo) || !candInfo.dli_fname) continue;
            char candLine[256];
            int candLen = snprintf(candLine, sizeof(candLine), "FAULT_CAND w%d=0x%lx %s+0x%lx",
                                   slot, candidate, candInfo.dli_fname,
                                   candidate - (unsigned long)(uintptr_t)candInfo.dli_fbase);
            if (candLen > 0) {
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", candLine);
            }
        }


        /* hilog is not async-signal-safe, so persist the report for the next launch to read. */
        int fd = open("/data/storage/el2/base/files/rea_fault.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t ignored = write(fd, buffer, (size_t)written);
            ignored = write(fd, "\n", 1);
            if (rawLength > 0) {
                ignored = write(fd, rawBuffer, (size_t)rawLength);
                ignored = write(fd, "\n", 1);
            }
            if (rawLength2 > 0) {
                ignored = write(fd, rawBuffer2, (size_t)rawLength2);
                ignored = write(fd, "\n", 1);
            }
            (void)ignored;
            close(fd);
        }
    }
    (void)faultAddress;
    _exit(128 + sig);
}

void InstallFaultHandler()
{
    // Normal execution belongs to the runtime/GC and Harmony's crash signal chain.
    // The old diagnostic handler allocates/logs while handling SIGSEGV and can
    // fault recursively inside malloc, hiding the original failure. Only the
    // explicitly requested calibration command may install it.
    if(!g_calibStart)return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = FaultHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGSYS, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);
}

/*
 * The engine's crash reporter installs its own handlers while graphics initialise, which would hide
 * the fault. A watchdog keeps re-asserting ours so the report survives whichever side installs last.
 */
void *ReassertHandlers(void *)
{
    for (;;) {
        InstallFaultHandler();
        usleep(20000);
    }
    return nullptr;
}

void StartFaultWatchdog()
{
    if(!g_calibStart){Emit("FAULT_HANDLER_OWNER runtime/system; diagnostic override disabled");return;}
    pthread_t thread;
    if (pthread_create(&thread, nullptr, ReassertHandlers, nullptr) == 0) {
        pthread_detach(thread);
    }
}

/* ---- fake JavaVM ------------------------------------------------------------------------- */

struct FakeEnv {
    void **functions;
};

FakeEnv g_env{ g_jni_table };
extern "C" void *ReaFrameworkEnv(){return &g_env;}
extern "C" void ReaLogJniState(){
    size_t objects=0,methods=0;uint64_t checks=0;
    {std::lock_guard<std::mutex> lock(g_objectMutex);objects=g_objects.size();methods=g_methods.size();checks=g_objectChecks;}
    Emit("JNI_REGISTRY objects=%zu methods=%zu lookups=%llu strings=%llu indexed=1",objects,methods,
         (unsigned long long)checks,(unsigned long long)g_stringConstructions.load(std::memory_order_relaxed));
}

extern "C" bool ReaDeliverSdkCallback(void *rawProxy,const char *result){
    // ReflectionHelper's long is the original strong GCHandle, not a guessed object address.
    if(!IsFakeObject(rawProxy)||!result)return false;
    auto *proxy=static_cast<FakeObject *>(rawProxy);
    uint64_t handle=proxy->ints["gchandle"];
    if(proxy->cls!="com/unity3d/player/ManagedProxy"||!handle||handle>UINT32_MAX)return false;
    void *library=dlopen("libil2cpp.so",RTLD_NOW|RTLD_NOLOAD);if(!library)return false;
    auto target=(void *(*)(uint32_t))dlsym(library,"il2cpp_gchandle_get_target");
    auto domain=(void *(*)())dlsym(library,"il2cpp_domain_get");
    auto assemblies=(const void **(*)(void *,size_t *))dlsym(library,"il2cpp_domain_get_assemblies");
    auto image=(void *(*)(const void *))dlsym(library,"il2cpp_assembly_get_image");
    auto imageName=(const char *(*)(void *))dlsym(library,"il2cpp_image_get_name");
    auto findClass=(void *(*)(void *,const char *,const char *))dlsym(library,"il2cpp_class_from_name");
    auto findMethod=(void *(*)(void *,const char *,int))dlsym(library,"il2cpp_class_get_method_from_name");
    auto invoke=(void *(*)(void *,void *,void **,void **))dlsym(library,"il2cpp_runtime_invoke");
    if(!target||!domain||!assemblies||!image||!imageName||!findClass||!findMethod||!invoke)return false;
    void *managed=target((uint32_t)handle);if(!managed)return false;
    void *method=nullptr;size_t count=0;auto **list=assemblies(domain(),&count);
    for(size_t i=0;i<count;++i){void *img=image(list[i]);const char *name=imageName(img);
        if(name&&strcmp(name,"UnityEngine.AndroidJNIModule.dll")==0){
            void *klass=findClass(img,"UnityEngine","_AndroidJNIHelper");
            if(klass)method=findMethod(klass,"InvokeJavaProxyMethod",3);break;
        }
    }
    if(!method){Emit("SDK_CALLBACK_ERROR missing original InvokeJavaProxyMethod");return false;}
    void *methodName=MakeStringObject("onResult");
    auto *arguments=NewObject("[Ljava/lang/Object;");arguments->ints["length"]=1;
    arguments->refs["[0]"]=MakeStringObject(result);
    void *javaArgs=arguments;void *parameters[]={managed,&methodName,&javaArgs};void *exception=nullptr;
    invoke(method,nullptr,parameters,&exception);
    if(exception){Emit("SDK_CALLBACK_ERROR original managed callback threw (payload withheld)");return false;}
    return true;
}
extern "C" void *ReaCreateDirectAudioBuffer(void *data,size_t length){
    auto *object=NewObject("java/nio/DirectByteBuffer");object->refs["direct"]=data;object->ints["capacity"]=length;return object;
}
extern "C" void ReaSetSystemLocale(const char *locale){
    if(locale&&*locale){g_systemLocale=locale;Emit("SYSTEM_LOCALE %s",locale);}
}
extern "C" void ReaSetNetworkType(int type){
    if(type<0||type>2)return;
    int before=g_networkType.exchange(type);
    if(before!=type)Emit("NETWORK_STATE originalUnityType=%d (0=offline 1=cellular 2=wifi/other)",type);
}
extern "C" void ReaSetSystemProxy(const char *text){std::lock_guard<std::mutex> lock(g_networkConfigMutex);g_systemProxy=text?text:"";}
extern "C" void ReaAddSystemTrustRoot(const unsigned char *data,size_t length){
    if(!data||length<256||length>16384||data[0]!=0x30)return;
    auto *cert=NewObject("java/security/cert/X509Certificate");cert->strs["der"].assign(reinterpret_cast<const char *>(data),length);
    std::lock_guard<std::mutex> lock(g_networkConfigMutex);g_systemRoots.push_back(cert);
}

static double FloatingResult(FakeObject *object,MethodRef *method,void *index)
{
    if (!object || !method) return 0;
    const auto &name=method->name;
    if (object->cls=="android/view/MotionEvent") {
        size_t point=(uintptr_t)index;
        if (name=="getRawX" || name=="getRawY")point=0;
        if (name=="getX" || name=="getRawX" || name=="getY" || name=="getRawY")
            return point<object->touchPoints.size() ? ((name=="getX" || name=="getRawX")?object->touchPoints[point].x:object->touchPoints[point].y) : 0;
        if (name=="getPressure" || name=="getSize" || name=="getXPrecision" || name=="getYPrecision")return 1;
        return 0;
    }
    if (name=="getRefreshRate")return ReaDisplayRefreshRate();
    auto found=object->numbers.find(name);
    if (found!=object->numbers.end())return found->second;
    if (name=="floatValue" || name=="doubleValue")return object->numbers["value"];
    return 0;
}
static double FloatingArguments(FakeObject *object,MethodRef *method,void *raw,bool array)
{
    void *first=nullptr,*second=nullptr;
    if (!FrameworkArguments(method,raw,array,&first,&second)) return 0;
    if(object&&object->preferences&&method&&method->name=="getFloat"){
        rea_prefs::Value value;uint32_t bits=(uintptr_t)second;float fallback;memcpy(&fallback,&bits,4);
        return rea_prefs::Read(object->preferences,ArgToString(first),value)&&value.type==rea_prefs::Type::Float?value.number:fallback;
    }
    return FloatingResult(object,method,first);
}
static float TypedFloatV(FakeEnv *,FakeObject *o,MethodRef *m,va_list a){return (float)FloatingArguments(o,m,&a,false);}
static double TypedDoubleV(FakeEnv *,FakeObject *o,MethodRef *m,va_list a){return FloatingArguments(o,m,&a,false);}
static float TypedFloatA(FakeEnv *,FakeObject *o,MethodRef *m,const void *a){return (float)FloatingArguments(o,m,(void *)a,true);}
static double TypedDoubleA(FakeEnv *,FakeObject *o,MethodRef *m,const void *a){return FloatingArguments(o,m,(void *)a,true);}
static float TypedFloat(FakeEnv *e,FakeObject *o,MethodRef *m,...){va_list a;va_start(a,m);float v=TypedFloatV(e,o,m,a);va_end(a);return v;}
static double TypedDouble(FakeEnv *e,FakeObject *o,MethodRef *m,...){va_list a;va_start(a,m);double v=TypedDoubleV(e,o,m,a);va_end(a);return v;}
static float NonvirtualFloatV(FakeEnv *e,FakeObject *o,void *,MethodRef *m,va_list a){return TypedFloatV(e,o,m,a);}
static double NonvirtualDoubleV(FakeEnv *e,FakeObject *o,void *,MethodRef *m,va_list a){return TypedDoubleV(e,o,m,a);}
static float NonvirtualFloatA(FakeEnv *e,FakeObject *o,void *,MethodRef *m,const void *a){return TypedFloatA(e,o,m,a);}
static double NonvirtualDoubleA(FakeEnv *e,FakeObject *o,void *,MethodRef *m,const void *a){return TypedDoubleA(e,o,m,a);}
static float NonvirtualFloat(FakeEnv *e,FakeObject *o,void *,MethodRef *m,...){va_list a;va_start(a,m);float v=TypedFloatV(e,o,m,a);va_end(a);return v;}
static double NonvirtualDouble(FakeEnv *e,FakeObject *o,void *,MethodRef *m,...){va_list a;va_start(a,m);double v=TypedDoubleV(e,o,m,a);va_end(a);return v;}
static double DoubleField(FakeEnv *,FakeObject *o,MethodRef *f)
{
    if(!o || !f)return 0;
    auto found=o->numbers.find(f->name);
    return found!=o->numbers.end()?found->second:(double)o->ints[f->name];
}
static float FloatField(FakeEnv *e,FakeObject *o,MethodRef *f){return (float)DoubleField(e,o,f);}
static void SetDoubleField(FakeEnv *,FakeObject *o,MethodRef *f,double v){if(o&&f)o->numbers[f->name]=v;}
static void SetFloatField(FakeEnv *e,FakeObject *o,MethodRef *f,float v){SetDoubleField(e,o,f,v);}
static void TypedVoid(FakeEnv *env,FakeObject *o,MethodRef *m,...){
    va_list args;va_start(args,m);ReaJniDispatch(62,env,o,m,&args,nullptr,nullptr,nullptr,nullptr);va_end(args);
}
static void InitializeTypedJni()
{
    // CallVoidMethod may carry ten parameters (Unity keyboard), including stack arguments.
    g_jni_table[61]=(void *)TypedVoid;g_jni_table[141]=(void *)TypedVoid;
    for(int start:{55,135}) {
        g_jni_table[start]=(void *)TypedFloat;g_jni_table[start+1]=(void *)TypedFloatV;g_jni_table[start+2]=(void *)TypedFloatA;
        g_jni_table[start+3]=(void *)TypedDouble;g_jni_table[start+4]=(void *)TypedDoubleV;g_jni_table[start+5]=(void *)TypedDoubleA;
    }
    g_jni_table[85]=(void *)NonvirtualFloat;g_jni_table[86]=(void *)NonvirtualFloatV;g_jni_table[87]=(void *)NonvirtualFloatA;
    g_jni_table[88]=(void *)NonvirtualDouble;g_jni_table[89]=(void *)NonvirtualDoubleV;g_jni_table[90]=(void *)NonvirtualDoubleA;
    for(int field:{102,152}){g_jni_table[field]=(void *)FloatField;g_jni_table[field+1]=(void *)DoubleField;}
    for(int field:{111,161}){g_jni_table[field]=(void *)SetFloatField;g_jni_table[field+1]=(void *)SetDoubleField;}
}

static bool VerifyJniIdentityContract()
{
    int first = 1, second = 2;
    using SameObjectFn = unsigned char (*)(FakeEnv *, void *, void *);
    auto same = reinterpret_cast<SameObjectFn>(g_jni_table[24]);
    bool ok = same(&g_env, &first, &first) && !same(&g_env, &first, &second)
        && same(&g_env, nullptr, nullptr) && !same(&g_env, &first, nullptr);
    Emit("JNI_IDENTITY_CONTRACT %s cases=4 (actual typed JNI table)", ok ? "PASS" : "FAIL");
    return ok;
}


static int TestIntV(FakeObject *receiver, MethodRef *method, ...)
{
    va_list list; va_start(list, method);
    using Fn=int (*)(FakeEnv *, void *, void *, va_list);
    int result=reinterpret_cast<Fn>(g_jni_table[50])(&g_env,receiver,method,list);
    va_end(list); return result;
}
static bool TestBoolV(FakeObject *receiver, MethodRef *method, ...)
{
    va_list list; va_start(list, method);
    using Fn=unsigned char (*)(FakeEnv *, void *, void *, va_list);
    bool result=reinterpret_cast<Fn>(g_jni_table[38])(&g_env,receiver,method,list)!=0;
    va_end(list); return result;
}
static void *TestObjectV(FakeObject *receiver, MethodRef *method, ...)
{
    va_list list; va_start(list, method);
    using Fn=void *(*)(FakeEnv *, void *, void *, va_list);
    void *result=reinterpret_cast<Fn>(g_jni_table[35])(&g_env,receiver,method,list);
    va_end(list); return result;
}
static bool VerifyFrameworkDefaultContract()
{
    auto *receiver=NewObject("android/content/SharedPreferences");
    auto *key=MakeStringObject("rea_contract_missing_key");
    auto *integer=NewMethod(receiver->cls,"getInt","(Ljava/lang/String;I)I",false,false);
    auto *boolean=NewMethod(receiver->cls,"getBoolean","(Ljava/lang/String;Z)Z",false,false);
    int checks=0,failed=0;
    auto check=[&](bool value){++checks;if(!value)++failed;};
    check(TestIntV(receiver,integer,key,37)==37);
    check(TestBoolV(receiver,boolean,key,1));
    rea_jni_args::JValue values[2]{};values[0].l=key;values[1].i=-13;
    using IntA=int (*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    check(reinterpret_cast<IntA>(g_jni_table[51])(&g_env,receiver,integer,values)==-13);
    values[1].z=1;
    using BoolA=unsigned char (*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    check(reinterpret_cast<BoolA>(g_jni_table[39])(&g_env,receiver,boolean,values)!=0);
    auto *iterator=NewObject("java/util/Iterator");
    check(!TestBoolV(iterator,NewMethod(iterator->cls,"hasNext","()Z",false,false)));
    auto *context=NewObject("android/content/Context");
    void *resource=TestObjectV(context,NewMethod(context->cls,"getString","(I)Ljava/lang/String;",false,false),123);
    check(IsFakeObject(resource) && static_cast<FakeObject *>(resource)->cls=="java/lang/String");
    Emit("JNI_FRAMEWORK_DEFAULTS %s checks=%d failed=%d (actual V/A JNI slots)",failed?"FAIL":"PASS",checks,failed);
    return failed==0;
}

static void *TestConstructV(FakeObject *clazz, MethodRef *method, ...)
{
    va_list list; va_start(list, method);
    using Fn=void *(*)(FakeEnv *,void *,void *,va_list);
    void *result=reinterpret_cast<Fn>(g_jni_table[29])(&g_env,clazz,method,list);
    va_end(list); return result;
}

static bool VerifyConstructionContract()
{
    int checks=0,failed=0;
    auto check=[&](bool okay){ ++checks; if (!okay) { ++failed; Emit("JNI_CONSTRUCTION_CASE failed=%d",checks); } };
    auto *clazz=NewObject("java/lang/Class"); clazz->strs["name"]="com/google/androidgamesdk/ChoreographerCallback";
    auto *ctor=NewMethod(clazz->strs["name"],"<init>","(J)V",false,false);
    constexpr int64_t cookie=0x12345678abcdefLL;
    auto *fromV=static_cast<FakeObject *>(TestConstructV(clazz,ctor,cookie));
    check(fromV && fromV->ints["cookie"]==cookie);
    rea_jni_args::JValue values[1]{}; values[0].j=cookie;
    using ConstructA=void *(*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    auto *fromA=static_cast<FakeObject *>(reinterpret_cast<ConstructA>(g_jni_table[30])(&g_env,clazz,ctor,values));
    check(fromA && fromA!=fromV && fromA->ints["cookie"]==cookie);
    using NewArray=void *(*)(FakeEnv *,int,void *,void *);
    using ArrayLength=int (*)(FakeEnv *,void *);
    auto *array=reinterpret_cast<NewArray>(g_jni_table[172])(&g_env,2,clazz,fromV);
    check(reinterpret_cast<ArrayLength>(g_jni_table[171])(&g_env,array)==2);
    using SetArray=void (*)(FakeEnv *,void *,int,void *);
    using GetArray=void *(*)(FakeEnv *,void *,int);
    reinterpret_cast<SetArray>(g_jni_table[174])(&g_env,array,1,fromA);
    check(reinterpret_cast<GetArray>(g_jni_table[173])(&g_env,array,0)==fromV);
    check(reinterpret_cast<GetArray>(g_jni_table[173])(&g_env,array,1)==fromA);
    reinterpret_cast<SetArray>(g_jni_table[174])(&g_env,array,1,nullptr);
    check(!reinterpret_cast<GetArray>(g_jni_table[173])(&g_env,array,1));
    using MethodID=void *(*)(FakeEnv *,void *,const char *,const char *);
    auto getMethod=reinterpret_cast<MethodID>(g_jni_table[33]);
    check(getMethod(&g_env,clazz,"<init>","(J)V")==ctor && getMethod(&g_env,clazz,"<init>","(J)V")==ctor);
    const char16_t text[]={0x4e2d,0x6587};
    using NewStringFn=void *(*)(FakeEnv *,const char16_t *,int);
    void *string=reinterpret_cast<NewStringFn>(g_jni_table[163])(&g_env,text,2);
    check(reinterpret_cast<ArrayLength>(g_jni_table[164])(&g_env,string)==2);
    check(reinterpret_cast<ArrayLength>(g_jni_table[168])(&g_env,string)==6);
    using StringChars=const char16_t *(*)(FakeEnv *,void *,unsigned char *);
    auto *chars=reinterpret_cast<StringChars>(g_jni_table[165])(&g_env,string,nullptr);
    check(chars && chars[0]==text[0] && chars[1]==text[1]);
    auto *uriClass=NewObject("java/lang/Class"); uriClass->strs["name"]="android/net/Uri";
    auto *encode=NewMethod("android/net/Uri","encode","(Ljava/lang/String;)Ljava/lang/String;",true,false);
    auto *decode=NewMethod("android/net/Uri","decode","(Ljava/lang/String;)Ljava/lang/String;",true,false);
    using ObjectA=void *(*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    auto invoke=reinterpret_cast<ObjectA>(g_jni_table[116]);
    values[0].l=MakeStringObject("/x +\xe4\xb8\xad");
    auto *encoded=static_cast<FakeObject *>(invoke(&g_env,uriClass,encode,values));
    check(encoded && encoded->strs["value"]=="%2Fx%20%2B%E4%B8%AD");
    values[0].l=encoded;
    auto *decoded=static_cast<FakeObject *>(invoke(&g_env,uriClass,decode,values));
    check(decoded && decoded->strs["value"]=="/x +\xe4\xb8\xad");
    values[0].l=nullptr; check(!invoke(&g_env,uriClass,encode,values));
    values[0].l=MakeStringObject("a+b");
    auto *plus=static_cast<FakeObject *>(invoke(&g_env,uriClass,decode,values));
    check(plus && plus->strs["value"]=="a+b");
    Emit("JNI_CONSTRUCTION_CONTRACT %s checks=%d failed=%d (actual V/A, arrays, UTF16 and static URI slots)",failed?"FAIL":"PASS",checks,failed);
    return failed==0;
}

static bool VerifyReflectionContract(){
    using ObjectA=void *(*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    auto invoke=reinterpret_cast<ObjectA>(g_jni_table[116]);
    auto *helper=NewObject("java/lang/Class");helper->strs["name"]="com/unity3d/player/ReflectionHelper";
    auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]="com/Exceed7/NativeAudio/NativeAudio";
    auto *lookup=NewMethod(helper->strs["name"],"getMethodID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;",true,false);
    rea_jni_args::JValue values[4]{};values[0].l=clazz;values[1].l=MakeStringObject("Initialize");values[2].l=MakeStringObject("(IIZ)I");values[3].z=1;
    auto *reflected=invoke(&g_env,helper,lookup,values);
    auto *method=reinterpret_cast<MethodRef *(*)(FakeEnv *,void *)>(g_jni_table[7])(&g_env,reflected);
    int checks=0,failed=0;auto check=[&](bool ok){++checks;if(!ok)++failed;};
    check(method&&method->cls==clazz->strs["name"]&&method->name=="Initialize");
    check(method&&method->signature=="(IIZ)I"&&method->isStatic);
    values[3].z=0;reflected=invoke(&g_env,helper,lookup,values);
    method=reinterpret_cast<MethodRef *(*)(FakeEnv *,void *)>(g_jni_table[7])(&g_env,reflected);
    check(method&&!method->isStatic);
    auto *fieldLookup=NewMethod(helper->strs["name"],"getFieldID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;",true,false);
    values[1].l=MakeStringObject("currentActivity");values[2].l=MakeStringObject("Landroid/app/Activity;");values[3].z=1;
    auto *field=invoke(&g_env,helper,fieldLookup,values);
    auto *id=reinterpret_cast<MethodRef *(*)(FakeEnv *,void *)>(g_jni_table[8])(&g_env,field);
    check(id&&id->isField&&id->isStatic&&id->signature=="Landroid/app/Activity;");
    auto *signature=NewMethod(helper->strs["name"],"getFieldSignature","(Ljava/lang/reflect/Field;)Ljava/lang/String;",true,false);
    values[0].l=field;check(ArgToString(invoke(&g_env,helper,signature,values))=="Landroid/app/Activity;");
    unsigned char buffer[64]{};
    auto *direct=ReaCreateDirectAudioBuffer(buffer,sizeof(buffer));
    check(reinterpret_cast<void *(*)(FakeEnv *,void *)>(g_jni_table[230])(&g_env,direct)==buffer);
    check(reinterpret_cast<int64_t (*)(FakeEnv *,void *)>(g_jni_table[231])(&g_env,direct)==sizeof(buffer));
    auto *bridgeClass=NewObject("java/lang/Class");bridgeClass->strs["name"]="com/tds/common/bridge/Bridge";
    values[0].l=bridgeClass;values[1].l=MakeStringObject("registerHandler");
    values[2].l=MakeStringObject("(Ljava/lang/String;Lcom.tds.common.bridge.BridgeCallback;)V");values[3].z=0;
    reflected=invoke(&g_env,helper,lookup,values);
    method=reinterpret_cast<MethodRef *(*)(FakeEnv *,void *)>(g_jni_table[7])(&g_env,reflected);
    rea_jni_args::Descriptor descriptor;std::string descriptorError;
    check(method&&method->signature=="(Ljava/lang/String;Lcom/tds/common/bridge/BridgeCallback;)V"&&
          rea_jni_args::Parse(method->signature.c_str(),descriptor,descriptorError)&&descriptor.parameters.size()==2);
    auto *callbackClass=NewObject("java/lang/Class");callbackClass->strs["name"]="com/tds/common/bridge/BridgeCallback";
    auto *getName=NewMethod("java/lang/Class","getName","()Ljava/lang/String;",false,false);
    check(ArgToString(DispatchObjectCall(callbackClass,getName,nullptr,nullptr))=="com.tds.common.bridge.BridgeCallback");
    auto *create=NewMethod(helper->strs["name"],"newProxyInstance","(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;",true,false);
    values[0].l=nullptr;values[1].j=0x101;values[2].l=callbackClass;
    auto *proxy=static_cast<FakeObject *>(invoke(&g_env,helper,create,values));
    check(proxy&&proxy->ints["gchandle"]==0x101&&proxy->refs["interface"]==callbackClass);
    auto *weak=reinterpret_cast<void *(*)(FakeEnv *,void *)>(g_jni_table[226])(&g_env,proxy);
    auto *local=reinterpret_cast<void *(*)(FakeEnv *,void *)>(g_jni_table[25])(&g_env,weak);
    check(weak==proxy&&local==proxy);
    check(!ReaSdkReply(UINT32_MAX,"{}",true));
    auto *deviceMethod=NewMethod(clazz->strs["name"],"GetDeviceAudioInformation","()Ljava/lang/Object;",true,false);
    auto *deviceInfo=static_cast<FakeObject *>(invoke(&g_env,clazz,deviceMethod,nullptr));
    check(deviceInfo&&deviceInfo->cls=="com/Exceed7/NativeAudio/NativeAudio$DeviceAudioInformation");
    auto *rate=NewMethod(deviceInfo->cls,"nativeSamplingRate","I",false,true);
    check((intptr_t)ReaJniDispatch(100,&g_env,deviceInfo,rate,nullptr,nullptr,nullptr,nullptr,nullptr)==48000);
    auto *frames=NewMethod(deviceInfo->cls,"optimalBufferSize","I",false,true);
    check((intptr_t)ReaJniDispatch(100,&g_env,deviceInfo,frames,nullptr,nullptr,nullptr,nullptr,nullptr)==192);
    auto *outputs=NewMethod(deviceInfo->cls,"outputDevices","[Landroid/media/AudioDeviceInfo;",false,true);
    auto *array=ReaJniDispatch(95,&g_env,deviceInfo,outputs,nullptr,nullptr,nullptr,nullptr,nullptr);
    check(array&&ReaJniDispatch(171,&g_env,array,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)==nullptr);
    auto *declaring=NewMethod("java/lang/reflect/Field","getDeclaringClass","()Ljava/lang/Class;",false,false);
    auto *reflectedField=NewObject("java/lang/reflect/Field");reflectedField->refs["field"]=outputs;
    auto *declared=static_cast<FakeObject *>(DispatchObjectCall(reflectedField,declaring,nullptr,nullptr));
    check(declared&&declared->strs["name"]==deviceInfo->cls);
    Emit("JNI_REFLECTION_AUDIO_CONTRACT %s checks=%d failed=%d",failed?"FAIL":"PASS",checks,failed);
    return failed==0;
}
static bool VerifyPreferenceContract(){
    int checks=0,failed=0;auto check=[&](bool okay){++checks;if(!okay)++failed;};
    auto *context=NewObject("android/content/Context");
    auto *open=NewMethod(context->cls,"getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",false,false);
    auto *testName=MakeStringObject("__rea_diagnostics_jni_contract");
    auto *prefs=static_cast<FakeObject *>(DispatchObjectCall(context,open,testName,nullptr));
    if(!prefs||!prefs->preferences)return false;
    auto *edit=NewMethod(prefs->cls,"edit","()Landroid/content/SharedPreferences$Editor;",false,false);
    auto *editor=static_cast<FakeObject *>(DispatchObjectCall(prefs,edit,nullptr,nullptr));
    auto *clear=NewMethod(editor->cls,"clear","()Landroid/content/SharedPreferences$Editor;",false,false);
    auto *commit=NewMethod(editor->cls,"commit","()Z",false,false);
    DispatchObjectCall(editor,clear,nullptr,nullptr);check(DispatchObjectCall(editor,commit,nullptr,nullptr)!=nullptr);
    auto *put=NewMethod(editor->cls,"putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",false,false);
    auto *get=NewMethod(prefs->cls,"getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",false,false);
    auto *key=MakeStringObject("text"),*text=MakeStringObject("\xe4\xb8\xad\xe6\x96\x87"),*fallback=MakeStringObject("missing");
    check(DispatchObjectCall(editor,put,key,text)==editor);
    check(DispatchObjectCall(prefs,get,key,fallback)==fallback); // edits are invisible until apply/commit
    check(DispatchObjectCall(editor,commit,nullptr,nullptr)!=nullptr);
    auto *second=static_cast<FakeObject *>(DispatchObjectCall(context,open,testName,nullptr));
    check(second&&ArgToString(DispatchObjectCall(second,get,key,fallback))==ArgToString(text));
    auto *putFloat=NewMethod(editor->cls,"putFloat","(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;",false,false);
    rea_jni_args::JValue arguments[2]{};arguments[0].l=MakeStringObject("float");arguments[1].f=0.875f;
    using ObjectA=void *(*)(FakeEnv *,void *,void *,const rea_jni_args::JValue *);
    check(reinterpret_cast<ObjectA>(g_jni_table[36])(&g_env,editor,putFloat,arguments)==editor);
    check(DispatchObjectCall(editor,commit,nullptr,nullptr)!=nullptr);
    auto *getAll=NewMethod(prefs->cls,"getAll","()Ljava/util/Map;",false,false);
    auto *map=static_cast<FakeObject *>(DispatchObjectCall(prefs,getAll,nullptr,nullptr));check(map&&map->items.size()==2);
    auto *entrySet=NewMethod("java/util/Map","entrySet","()Ljava/util/Set;",false,false);
    auto *set=static_cast<FakeObject *>(DispatchObjectCall(map,entrySet,nullptr,nullptr));
    auto *iteratorMethod=NewMethod("java/util/Set","iterator","()Ljava/util/Iterator;",false,false);
    auto *iterator=static_cast<FakeObject *>(DispatchObjectCall(set,iteratorMethod,nullptr,nullptr));
    auto *has=NewMethod("java/util/Iterator","hasNext","()Z",false,false);
    auto *next=NewMethod("java/util/Iterator","next","()Ljava/lang/Object;",false,false);
    int found=0;while(DispatchObjectCall(iterator,has,nullptr,nullptr)&&found<3){
        auto *entry=static_cast<FakeObject *>(DispatchObjectCall(iterator,next,nullptr,nullptr));
        check(entry&&entry->refs["key"]&&entry->refs["value"]);++found;
    }check(found==2);
    DispatchObjectCall(editor,clear,nullptr,nullptr);check(DispatchObjectCall(editor,commit,nullptr,nullptr)!=nullptr);
    Emit("JNI_PREFERENCES_CONTRACT %s checks=%d failed=%d",failed?"FAIL":"PASS",checks,failed);return !failed;
}
static float TestFloatV(FakeObject *o,MethodRef *m,...)
{
    va_list a;va_start(a,m);
    using Fn=float (*)(FakeEnv *,FakeObject *,MethodRef *,va_list);
    float result=((Fn)g_jni_table[56])(&g_env,o,m,a);va_end(a);return result;
}
static bool VerifyFloatingContract()
{
    int checks=0,failed=0;
    auto check=[&](bool okay){++checks;if(!okay)++failed;};
    auto *motion=NewObject("android/view/MotionEvent");motion->touchPoints={{7,125.5f,-64.25f},{9,-900.5f,300.125f}};
    auto *x=NewMethod(motion->cls,"getX","(I)F",false,false);
    auto *y=NewMethod(motion->cls,"getY","(I)F",false,false);
    using Float=float (*)(FakeEnv *,FakeObject *,MethodRef *,...);
    check(((Float)g_jni_table[55])(&g_env,motion,x,0)==125.5f);
    check(TestFloatV(motion,x,1)==-900.5f);
    rea_jni_args::JValue index[1]{};index[0].i=1;
    using FloatA=float (*)(FakeEnv *,FakeObject *,MethodRef *,const void *);
    check(((FloatA)g_jni_table[57])(&g_env,motion,y,index)==300.125f);
    auto *field=NewMethod("android/util/DisplayMetrics","density","F",false,true);
    auto *metrics=NewObject("android/util/DisplayMetrics");metrics->numbers["density"]=2.25;
    using FloatFieldFn=float (*)(FakeEnv *,FakeObject *,MethodRef *);
    check(((FloatFieldFn)g_jni_table[102])(&g_env,metrics,field)==2.25f);
    using SetFloatFn=void (*)(FakeEnv *,FakeObject *,MethodRef *,float);
    ((SetFloatFn)g_jni_table[111])(&g_env,metrics,field,-10.5f);
    check(((FloatFieldFn)g_jni_table[102])(&g_env,metrics,field)==-10.5f);
    metrics->numbers["precise"]=1000000.125;
    auto *precise=NewMethod(metrics->cls,"precise","D",false,true);
    using DoubleFieldFn=double (*)(FakeEnv *,FakeObject *,MethodRef *);
    check(((DoubleFieldFn)g_jni_table[103])(&g_env,metrics,precise)==1000000.125);
    auto *display=NewObject("android/view/Display");
    check(TestFloatV(display,NewMethod(display->cls,"getRefreshRate","()F",false,false))==(float)ReaDisplayRefreshRate());
    Emit("JNI_FLOATING_CONTRACT %s checks=%d failed=%d (actual FP return registers, V/A coordinates and fields)",failed?"FAIL":"PASS",checks,failed);
    return failed==0;
}

void *VmAttach(void *vm, void **outEnv, void *args)
{
    (void)vm; (void)args;
    if (outEnv) *outEnv = &g_env;
    return (void *)(intptr_t)0;
}

void *VmGetEnv(void *vm, void **outEnv, int version)
{
    (void)vm; (void)version;
    if (outEnv) *outEnv = &g_env;
    return (void *)(intptr_t)0;
}

void *VmDetach(void *vm) { (void)vm; return (void *)(intptr_t)0; }
void *VmDestroy(void *vm) { (void)vm; return (void *)(intptr_t)0; }

struct FakeVm {
    void **functions;
};

FakeVm g_vm{ nullptr };

void *g_vmFunctions[8] = {
    nullptr, nullptr, nullptr,
    (void *)&VmDestroy,
    (void *)&VmAttach,
    (void *)&VmDetach,
    (void *)&VmGetEnv,
    (void *)&VmAttach,
};

} // namespace

void *ReaFakeVmHandle()
{
    return &g_vm;
}

/* ---- bootstrap --------------------------------------------------------------------------- */

namespace {

struct RegisteredNative {
    std::string className;
    std::string name;
    std::string signature;
    void *fnPtr;
};

std::vector<RegisteredNative> g_registered;

void CaptureRegistrations(FakeObject *clazz, void *methods, int count)
{
    struct JNINativeMethod { const char *name; const char *signature; void *fnPtr; };
    JNINativeMethod *table = (JNINativeMethod *)methods;
    for (int i = 0; i < count && table; i++) {
        RegisteredNative entry;
        entry.className = clazz ? clazz->strs["name"] : "";
        entry.name = table[i].name ? table[i].name : "";
        entry.signature = table[i].signature ? table[i].signature : "";
        entry.fnPtr = table[i].fnPtr;
        g_registered.push_back(entry);
    }
}

static bool HandleNativeDialogCall(MethodRef *m,const rea_jni_args::Arguments &a){
    if(m->cls!="com/pingak9/nativepopup/Bridge")return false;
    if(m->name=="DismissCurrentAlert"){ReaDismissNativeDialog();return true;}
    ReaDialogState state;size_t buttons=0;
    if(m->name=="ShowDialogInfo"&&a.size()==3){state.kind=0;state.cancelable=false;buttons=1;}
    else if(m->name=="ShowDialogConfirm"&&(a.size()==4||a.size()==5)){state.kind=1;state.cancelable=a.size()==4||a[4].value.z;buttons=2;}
    else if(m->name=="ShowDialogNeutral"&&a.size()==5){state.kind=2;state.cancelable=true;buttons=3;}
    else return false;
    state.title=ArgToString(a[0].value.l);state.message=ArgToString(a[1].value.l);
    for(size_t i=0;i<buttons;++i)state.buttons.push_back(ArgToString(a[2+i].value.l));
    ReaShowNativeDialog(state);return true;
}
static void DrainNativeDialogs(){
    for(const auto &result:ReaTakeDialogResults()){
        for(const auto &native:g_registered)if(native.className=="com/unity3d/player/UnityPlayer"&&native.name=="nativeUnitySendMessage"&&
                native.signature=="(Ljava/lang/String;Ljava/lang/String;[B)V"){
            auto *bytes=NewObject("[B");bytes->strs["bytes"]=result.argument;
            auto *clazz=NewObject("java/lang/Class");clazz->strs["name"]="com/unity3d/player/UnityPlayer";
            ((void (*)(void *,void *,void *,void *,void *))native.fnPtr)(ReaFrameworkEnv(),clazz,MakeStringObject(result.target.c_str()),MakeStringObject(result.method.c_str()),bytes);
            Emit("NATIVE_DIALOG_CALLBACK delivered %s.%s",result.target.c_str(),result.method.c_str());break;
        }
    }
}
static bool HandleKeyboardCall(MethodRef *m,const rea_jni_args::Arguments &a){
    if(m->name=="hideSoftInput"){ReaHideKeyboard();return true;}
    if(m->name=="setSoftInputStr"&&a.size()==1){ReaChangeKeyboard(ArgToString(a[0].value.l).c_str());return true;}
    if(m->name=="setSelection"&&a.size()==2){ReaChangeKeyboard(nullptr,-1,a[0].value.i,a[1].value.i);return true;}
    if(m->name=="setCharacterLimit"&&a.size()==1){ReaChangeKeyboard(nullptr,a[0].value.i);return true;}
    if(m->name=="showSoftInput"&&a.size()==10&&m->signature=="(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V"){
        ReaKeyboardState state;state.text=ArgToString(a[0].value.l);state.type=a[1].value.i;
        state.multiline=a[3].value.z;state.secure=a[4].value.z;state.hint=ArgToString(a[6].value.l);
        state.limit=std::max(0,a[7].value.i);state.start=std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t>{}.from_bytes(state.text).size();
        ReaShowKeyboard(state);return true;
    }
    return false;
}
static void DrainKeyboard(FakeObject *player){
    auto call=[&](const char *name,const char *signature,auto... args){
        for(const auto &entry:g_registered)if(entry.className=="com/unity3d/player/UnityPlayer"&&entry.name==name&&entry.signature==signature){
            ((void (*)(void *,void *,decltype(args)...))entry.fnPtr)(ReaFrameworkEnv(),player,args...);return;
        }
        Emit("IME_CALLBACK_MISSING %s",name);
    };
    for(const auto &e:ReaTakeKeyboardEvents()){
        if(e.status==3){call("nativeReportKeyboardConfigChanged","()V");call("nativeSetKeyboardIsVisible","(Z)V",(unsigned char)1);continue;}
        if(e.status==2)call("nativeSoftInputCanceled","()V");
        else {call("nativeSetInputString","(Ljava/lang/String;)V",(void *)MakeStringObject(e.text.c_str()));
              call("nativeSetInputSelection","(II)V",e.start,e.length);}
        if(e.status){call("nativeSetKeyboardIsVisible","(Z)V",(unsigned char)0);call("nativeSoftInputClosed","()V");
                    Emit("IME_CLOSED status=%d (text withheld)",e.status);}
    }
}
static void *InvokeJavaProxy(FakeObject *proxy, const char *method, FakeObject *argument)
{
    if (!proxy || proxy->cls != "bitter/jnibridge/Proxy" || !proxy->ints["cookie"]) return nullptr;
    // Use the callback registered by the original player and its original cookie.
    // No invented engine object, copied vtable, or executable-code patch is used.
    void *invoke = nullptr;
    for (const auto &entry : g_registered)
        if (entry.className == "bitter/jnibridge/JNIBridge" && entry.name == "invoke" &&
            entry.signature == "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;")
            invoke = entry.fnPtr;
    if (!invoke) { Emit("JNIBRIDGE_MISSING invoke"); return nullptr; }
    auto *interfaces = static_cast<FakeObject *>(proxy->refs["interfaces"]);
    const char *wanted = strcmp(method, "run") == 0 ? "java/lang/Runnable" :
                         strcmp(method, "handleMessage") == 0 ? "android/os/Handler$Callback" :
                         strcmp(method,"onFrameAvailable")==0 ? "android/graphics/SurfaceTexture$OnFrameAvailableListener" :
                         "android/view/Choreographer$FrameCallback";
    FakeObject *declaringClass = nullptr;
    if (interfaces) for (const auto &item : interfaces->refs) {
        auto *entry = static_cast<FakeObject *>(item.second);
        if (entry && entry->strs["name"] == wanted) declaringClass = entry;
    }
    if (!declaringClass) { Emit("JNIBRIDGE_MISSING interface=%s", wanted); return nullptr; }
    const char *signature = strcmp(method, "run") == 0 ? "()V" :
                            strcmp(method, "handleMessage") == 0 ? "(Landroid/os/Message;)Z" :
                            strcmp(method,"onFrameAvailable")==0 ? "(Landroid/graphics/SurfaceTexture;)V" : "(J)V";
    auto *reflected = NewObject("java/lang/reflect/Method");
    reflected->refs["method"] = NewMethod(wanted, method, signature, false, false);
    auto *arguments = NewObject("[Ljava/lang/Object;");
    arguments->ints["length"] = argument ? 1 : 0;
    if (argument) arguments->refs["[0]"] = argument;
    using Invoke = void *(*)(void *, void *, int64_t, void *, void *, void *);
    if (strcmp(method, "doFrame") != 0) Emit("JNIBRIDGE_DELIVER %s cookie=0x%llx", method, (long long)proxy->ints["cookie"]);
    return ((Invoke)invoke)(&g_env, nullptr, proxy->ints["cookie"], declaringClass, reflected, arguments);
}

static FakeObject *PrepareJavaLoop()
{
    if (!g_threadLooper) {
        g_threadLooper = NewObject("android/os/Looper");
        g_threadLooper->loop = std::make_shared<JavaLoop>();
        Emit("JAVA_LOOP_PREPARE tid=%d", (int)syscall(SYS_gettid));
    }
    return g_threadLooper;
}

static FakeObject *GetMainJavaLoop()
{
    static FakeObject *mainLoop = [] {
        auto *looper = NewObject("android/os/Looper");
        looper->loop = std::make_shared<JavaLoop>();
        pthread_t thread;
        int result = pthread_create(&thread, nullptr, [](void *raw) -> void * {
            g_threadLooper = static_cast<FakeObject *>(raw);
            pthread_setname_np(pthread_self(), "ReaJavaMain");
            Emit("JAVA_MAIN_LOOP tid=%d", (int)syscall(SYS_gettid));
            RunJavaLoop(g_threadLooper);
            return nullptr;
        }, looper);
        if (result == 0) pthread_detach(thread);
        else { Emit("JAVA_MAIN_LOOP failed=%d", result); return (FakeObject *)nullptr; }
        return looper;
    }();
    return mainLoop;
}

static void QueueJavaTask(FakeObject *looper, FakeObject *callback, const char *method,
                          FakeObject *argument, int64_t delayMs)
{
    if (!looper || !looper->loop || !callback) { Emit("JAVA_QUEUE_REJECT method=%s missing looper/callback", method); return; }
    auto loop = looper->loop;
    auto delay=std::chrono::nanoseconds(std::max<int64_t>(delayMs,0)*1000000);
    if(strcmp(method,"doFrame")==0)delay=std::chrono::nanoseconds(ReaNextFrameDelayNs(delay.count()));
    { std::lock_guard<std::mutex> lock(loop->mutex);
      loop->tasks.push_back({callback, method, argument, std::chrono::steady_clock::now() + delay}); }
    loop->changed.notify_one();
}

static void RunJavaLoop(FakeObject *looper)
{
    if (!looper || !looper->loop) return;
    auto loop = looper->loop;
    for (;;) {
        JavaTask task;
        { std::unique_lock<std::mutex> lock(loop->mutex);
          if (loop->quit) return;
          if (loop->tasks.empty()) { loop->changed.wait(lock); continue; }
          auto next = std::min_element(loop->tasks.begin(), loop->tasks.end(), [](const JavaTask &a, const JavaTask &b) { return a.due < b.due; });
          if (next->due > std::chrono::steady_clock::now()) { loop->changed.wait_until(lock, next->due); continue; }
          task = *next; loop->tasks.erase(next); }
        if (task.method == "doFrame") {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            task.argument = NewObject("java/lang/Long");
            task.argument->ints["value"] = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
            static std::atomic<unsigned> frames{0};
            unsigned frame = ++frames;
            if (frame <= 3 || frame % 300 == 0) Emit("JAVA_CHOREOGRAPHER frame=%u tid=%d monotonic_ns=%lld", frame, (int)syscall(SYS_gettid), (long long)task.argument->ints["value"]);
        }
        InvokeJavaProxy(task.callback, task.method.c_str(), task.argument);
    }
}

static void VideoFrameAvailable(void *raw){
    auto *texture=static_cast<FakeObject *>(raw);FakeObject *listener=nullptr,*looper=nullptr;
    {std::lock_guard<std::mutex> lock(g_videoJniMutex);listener=static_cast<FakeObject *>(texture->refs["frameListener"]);looper=static_cast<FakeObject *>(texture->refs["frameLooper"]);}
    if(listener)QueueJavaTask(looper?looper:GetMainJavaLoop(),listener,"onFrameAvailable",texture);
}
extern "C" bool ReaShim_JavaSurfaceWindow(void *raw,void **window){
    if(!window||!IsFakeObject(raw))return false;auto *object=static_cast<FakeObject *>(raw);
    std::lock_guard<std::mutex> lock(g_videoJniMutex);
    if(object->cls=="android/view/Surface"){
        if(object->nativeSurface){*window=object->nativeSurface->window;return true;}
        if(!object->ints["videoSurface"])return false;
        if(object->ints["released"]){*window=nullptr;return true;}
        object=static_cast<FakeObject *>(object->refs["surfaceTexture"]);
    }
    if(!object||object->cls!="android/graphics/SurfaceTexture")return false;
    *window=ReaVideoTextureWindow(object->refs["nativeTexture"]);return true;
}
static void *JavaThreadBody(void *raw)
{
    auto *thread = static_cast<FakeObject *>(raw);
    pthread_setname_np(pthread_self(), "ReaJavaThread");
    InvokeJavaProxy(static_cast<FakeObject *>(thread->refs["runnable"]), "run", nullptr);
    return nullptr;
}

} // namespace

/* Exposed to probe.cpp: the whole bootstrap, driven from ArkTS. */
/*
 * HarmonyOS keeps a HAP's rawfile somewhere under the installed bundle directory, but the exact
 * layout is not documented as a stable path. The engine needs ordinary file descriptors for
 * pread(), so the location has to be found rather than assumed: this tries the plausible roots
 * against a file that is known to be packaged and reports which one opens.
 */
extern "C" const char *ReaPathProbe()
{
    g_log.clear();
    Emit("=== rawfile path probe ===");

    const char *candidates[] = {
        "/data/storage/el1/bundle/resources/rawfile/assets",
        "/data/storage/el1/bundle/resources/rawfile",
        "/data/storage/el1/bundle/entry/resources/rawfile/assets",
        "/data/storage/el1/bundle/entry/resources/rawfile",
        "/data/storage/el1/bundle/assets",
        "/data/storage/el1/base/resources/rawfile/assets",
        "/data/storage/el1/base/resources/rawfile",
    };

    for (const char *root : candidates) {
        std::string probe = std::string(root) + "/assetprobe.txt";
        int fd = open(probe.c_str(), O_RDONLY);
        if (fd >= 0) {
            char content[64] = {0};
            ssize_t got = read(fd, content, sizeof(content) - 1);
            close(fd);
            Emit("FOUND %s (read %d bytes: \"%s\")", root, (int)got, content);
        } else {
            Emit("missing %s", root);
        }
    }

    /* Also report what the app can see at the bundle root, to avoid guessing next time. */
    const char *dirs[] = {"/data/storage/el1/bundle", "/data/storage/el2/base"};
    for (const char *dir : dirs) {
        DIR *handle = opendir(dir);
        if (!handle) {
            Emit("opendir(%s) failed", dir);
            continue;
        }
        Emit("listing %s:", dir);
        struct dirent *entry;
        int shown = 0;
        while ((entry = readdir(handle)) != nullptr && shown < 16) {
            Emit("   %s", entry->d_name);
            shown++;
        }
        closedir(handle);
    }

    Emit("=== rawfile path probe end ===");
    return g_log.c_str();
}

/*
 * Unity installs its own fatal-signal handler during engine startup and writes a tombstone file,
 * which overrides the handler in this module. That file lives inside the app sandbox and is readable
 * here, so a crash from the previous launch can be reported on the next one - the only crash channel
 * available without access to HarmonyOS fault logs.
 */
extern "C" const char *ReaDumpTombstone()
{
    g_log.clear();
    const char *candidates[] = {
        "/data/storage/el2/base/files/rea_fault.txt",
        "/data/storage/el2/base/files/tombstone_00",
        "/data/storage/el2/base/files/tombstone_01",
        "/data/storage/el2/base/cache/tombstone_00",
        "/data/storage/el2/base/files/Unity/tombstone_00",
        "/data/storage/el2/base/cache/Unity/tombstone_00",
        "/data/storage/el2/base/files/tombstone_00.txt",
    };

    Emit("=== tombstone scan ===");
    bool found = false;
    for (const char *path : candidates) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        found = true;
        Emit("--- %s", path);
        char buffer[4096];
        ssize_t got = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (got > 0) {
            buffer[got] = '\0';
            /* hilog truncates long lines, so emit the tombstone line by line. */
            char *cursor = buffer;
            while (*cursor) {
                char *newline = strchr(cursor, '\n');
                if (newline) *newline = '\0';
                Emit("T| %s", cursor);
                if (!newline) break;
                cursor = newline + 1;
            }
        }
    }
    if (!found) Emit("no tombstone found");

    /* Also list the files directory so a differently named dump is not missed. */
    DIR *dir = opendir("/data/storage/el2/base/files");
    if (dir) {
        Emit("files dir listing:");
        struct dirent *entry;
        int shown = 0;
        while ((entry = readdir(dir)) != nullptr && shown < 24) {
            Emit("   %s", entry->d_name);
            shown++;
        }
        closedir(dir);
    }
    Emit("=== tombstone scan end ===");
    return g_log.c_str();
}

/*
 * Standalone IL2CPP bring-up: extract the metadata to a real path and initialise the runtime
 * without involving the engine, so a failure here is clearly attributable.
 */
extern "C" const char *ReaIl2CppProbe()
{
    g_log.clear();
    Emit("=== il2cpp standalone probe ===");

    const char *metadataAsset = "bin/Data/Managed/Metadata/global-metadata.dat";
    const char *cacheDir = "/data/storage/el2/base/cache";
    const char *metadataPath = "/data/storage/el2/base/cache/global-metadata.dat";

    int fd = -1;
    int64_t offset = 0;
    int64_t length = 0;
    if (!ReaAssetOpen(metadataAsset, &fd, &offset, &length)) {
        Emit("cannot resolve %s in the HAP", metadataAsset);
        return g_log.c_str();
    }
    Emit("metadata source fd=%d offset=%lld length=%lld", fd, (long long)offset, (long long)length);

    mkdir(cacheDir, 0700);
    int out = open(metadataPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        Emit("cannot create %s", metadataPath);
        return g_log.c_str();
    }
    char buffer[65536];
    int64_t remaining = length;
    int64_t cursor = offset;
    bool copied = true;
    while (remaining > 0) {
        size_t chunk = remaining < (int64_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t got = pread(fd, buffer, chunk, (off_t)cursor);
        if (got <= 0) {
            copied = false;
            break;
        }
        ssize_t written = write(out, buffer, (size_t)got);
        if (written != got) {
            copied = false;
            break;
        }
        cursor += got;
        remaining -= got;
    }
    close(out);
    if (!copied) {
        Emit("metadata copy failed with %lld bytes left", (long long)remaining);
        return g_log.c_str();
    }
    Emit("metadata extracted to %s", metadataPath);

    void *il2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!il2cpp) {
        Emit("dlopen libil2cpp.so failed: %s", dlerror());
        return g_log.c_str();
    }

    using SetDataDirFn = void (*)(const char *);
    using InitFn = int (*)(const char *);
    SetDataDirFn setDataDir = (SetDataDirFn)dlsym(il2cpp, "il2cpp_set_data_dir");
    InitFn init = (InitFn)dlsym(il2cpp, "il2cpp_init");
    SetDataDirFn setConfigDir = (SetDataDirFn)dlsym(il2cpp, "il2cpp_set_config_dir");
    if (!setDataDir || !init) {
        Emit("il2cpp entry points missing (set_data_dir=%p init=%p)", (void *)setDataDir, (void *)init);
        return g_log.c_str();
    }

    if (setConfigDir) setConfigDir(cacheDir);
    /* IL2CPP wants the directory that CONTAINS the metadata file. */
    setDataDir(cacheDir);
    Emit("calling il2cpp_init(\"Phigros\") ...");
    int result = init("Phigros");
    Emit("il2cpp_init returned %d %s", result, result ? "(ok)" : "(failed)");

    using DomainFn = void *(*)(void);
    DomainFn domainGet = (DomainFn)dlsym(il2cpp, "il2cpp_domain_get");
    if (domainGet) {
        void *domain = domainGet();
        Emit("il2cpp_domain_get -> %p", domain);
    }
    Emit("=== il2cpp standalone probe end ===");
    return g_log.c_str();
}

/*
 * Deliberate, fully instrumented crash used to measure the ucontext_t layout.
 *
 * A fixed-offset guess could not be trusted, so the crashing frame publishes the address of its own
 * local immediately before dereferencing null. The handler's dump then shows which word holds an
 * address next to that local, which identifies the real sp index; pc and lr follow from sigcontext.
 */
extern "C" void ReaCalibrateCrash(void)
{
    InstallFaultHandler();
    StartFaultWatchdog();

    volatile unsigned long marker = 0x1122334455667788UL;
    char local[64];
    local[0] = 1;
    g_calibStart = (unsigned long)(uintptr_t)&ReaCalibrateCrash;
    char line[224];
    snprintf(line, sizeof(line), "CALIB local=0x%lx marker=0x%lx fn=0x%lx",
             (unsigned long)(uintptr_t)local, (unsigned long)(uintptr_t)&marker, g_calibStart);
    OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", line);

    volatile int *nowhere = nullptr;
    *nowhere = 1;
}

extern "C" const char *ReaJniProbe()
{
    InitializeTypedJni();
    g_log.clear();
    g_slotsHit.clear();
    g_registered.clear();
    g_vm.functions = g_vmFunctions;
    Emit("=== JNI_OnLoad probe ===");

    void *main = dlopen("libmain.so", RTLD_NOW | RTLD_GLOBAL);
    if (!main) {
        const char *error = dlerror();
        Emit("LOAD_FAIL libmain.so: %s", error ? error : "?");
        return g_log.c_str();
    }
    using JniOnLoadFn = int (*)(void *, void *);
    JniOnLoadFn onLoad = (JniOnLoadFn)dlsym(main, "JNI_OnLoad");
    if (!onLoad) {
        Emit("SYM_MISSING JNI_OnLoad");
        return g_log.c_str();
    }
    int version = onLoad(&g_vm, nullptr);
    Emit("JNI_OnLoad returned 0x%x -> registered %zu natives", version, g_registered.size());
    return g_log.c_str();
}

/* ---- traced child execution -------------------------------------------------------------- */

/*
 * Runs one engine call in a forked child while this process traces it. The tracer observes the fatal
 * signal before any handler (ours or the engine's) can act, and PTRACE_GETREGSET yields the real
 * register file, so the faulting instruction and the null register are known exactly.
 */
namespace {

struct TracedCall {
    void *target;
    void *env;
    void *receiver;
    int arg;
    void *objectArg;
};

void ChildBody(void *parameter)
{
    TracedCall *call = (TracedCall *)parameter;
    using Fn = void (*)(void *, void *, int, void *);
    ((Fn)call->target)(call->env, call->receiver, call->arg, call->objectArg);
    _exit(0);
}

/* Returns true when the child died from a fatal signal, false when the call completed. */
/* Reads the child's registers through ptrace. */
static bool GetChildRegs(pid_t child, struct user_pt_regs *regs)
{
    struct iovec io;
    io.iov_base = regs;
    io.iov_len = sizeof(*regs);
    return ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &io) == 0;
}

/* Waits for the child, interrupting it when it makes no progress for a minute. */
static int WaitChildOrInterrupt(pid_t child, int *status)
{
    for (int waited = 0; waited < 600; waited++) {
        errno = 0;
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child) return 0;
        if (result < 0 && errno != EINTR) return -1;
        usleep(100000);
    }
    ptrace(PTRACE_INTERRUPT, child, 0, 0);
    for (int waited = 0; waited < 50; waited++) {
        errno = 0;
        if (waitpid(child, status, WNOHANG) == child) return 1;
        usleep(100000);
    }
    return -1;
}

/* Runs the engine's own SetWindow step before the first frame. */
static void CallSetWindow(void)
{
    if (!g_unityBase) return;
    using SetWindowFn = void (*)(void);
    unsigned char *guard = (unsigned char *)(uintptr_t)(g_unityBase + 0x1347a1cUL);
    Emit("SETWINDOW guard was %d, set to 1", (int)*guard);
    *guard = 1;

    /* The setter only acts when this state enum is 2..4. */
    void **stateSlot = (void **)(uintptr_t)(g_unityBase + 0x133bed0UL);
    void *stateObject = *stateSlot;
    if (stateObject) {
        int state = *(int *)stateObject;
        Emit("SETWINDOW state object %p value=%d", stateObject, state);
        if (state < 2 || state > 4) {
            *(int *)stateObject = 2;
            Emit("SETWINDOW state forced to 2");
        }
    } else {
        Emit("SETWINDOW state object is null");
    }

    Emit("SETWINDOW calling libunity+0x73b468");
    ((SetWindowFn)(uintptr_t)(g_unityBase + 0x73b468UL))();
    Emit("SETWINDOW returned");
}

/* Clears the guard byte that gates the blocking frame-sync call. */
static void SkipFrameSync(void)
{
    if (!g_unityBase) return;
    unsigned char *guard = (unsigned char *)(uintptr_t)(g_unityBase + 0x13ce458UL);
    Emit("FRAME_SYNC guard was %d", (int)*guard);
    *guard = 0;
}

/* Fills the async result slot that the render loop's lazy initialiser waits for. */
static void FillAsyncResultSlot(void)
{
    if (!g_unityBase) return;
    unsigned long *slot = (unsigned long *)(uintptr_t)(g_unityBase + 0x13ce0c0UL);
    if (*slot != 0) {
        Emit("ASYNC_SLOT already set: 0x%lx", *slot);
        return;
    }
    void *placeholder = calloc(1, 0x400);
    if (!placeholder) return;
    *slot = (unsigned long)(uintptr_t)placeholder;
    Emit("ASYNC_SLOT filled with zeroed block at %p", placeholder);
}

/* Registry entries are real Unity managers, not nullable work buffers.
 * The original table has 24 object slots followed by type records. Never write
 * zeroed placeholders here: MonoManager+0x310 must be constructed by the player.
 */
static void InstallRegistryPlaceholder(void)
{
    Emit("REGISTRY placeholders disabled; original player must construct managers");
}

/* Number of live threads in this process, used to see whether engine threads were created. */
static int CountThreads(void)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return -1;
    int count = 0;
    while (struct dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        count++;
    }
    closedir(dir);
    return count;
}

extern "C" void ReaStartCondRescue(void);

bool RunTraced(TracedCall *call);

/* Watchdog state: the worker's identity, whether a frame is running, and where it was interrupted. */
static pthread_t g_frameThread;
static std::atomic<bool> g_frameThreadValid{false};
static std::atomic<bool> g_frameInProgress{false};
static std::atomic<uint64_t> g_frameSerial{0};
static volatile unsigned long g_hangPc = 0;
static volatile unsigned long g_hangLr = 0;
static volatile unsigned long g_hangSp = 0;
static volatile unsigned long g_hangArg0 = 0;
static volatile unsigned long g_hangArg1 = 0;
static volatile unsigned long g_hangArg2 = 0;
static volatile bool g_hangReported = false;

static void DumpThreads(void);

static void HangSignalHandler(int signalNumber, siginfo_t *info, void *context)
{
    (void)signalNumber;
    (void)info;
    ucontext_t *ucontext = (ucontext_t *)context;
    g_hangPc = (unsigned long)ucontext->uc_mcontext.pc;
    g_hangLr = (unsigned long)ucontext->uc_mcontext.regs[30];
    g_hangSp = (unsigned long)ucontext->uc_mcontext.sp;
    g_hangArg0 = (unsigned long)ucontext->uc_mcontext.regs[0];
    g_hangArg1 = (unsigned long)ucontext->uc_mcontext.regs[1];
    g_hangArg2 = (unsigned long)ucontext->uc_mcontext.regs[2];
}

static void *HangWatchdog(void *raw)
{
    (void)raw;
    for (;;) {
        sleep(1);
        if (!g_frameInProgress || g_hangReported || !g_frameThreadValid) continue;
        static int waited = 0;
        static uint64_t observedFrame=~0ULL;
        uint64_t currentFrame=g_frameSerial.load();
        if(currentFrame!=observedFrame){observedFrame=currentFrame;waited=0;}
        if (++waited < 15) continue;
        waited = 0;
        g_hangPc = 0;
        pthread_kill(g_frameThread, SIGUSR1);
        usleep(200000);
        if (g_hangPc != 0) {
            Dl_info where;
            memset(&where, 0, sizeof(where));
            const char *module = "?";
            unsigned long offset = 0;
            if (dladdr((void *)(uintptr_t)g_hangPc, &where) && where.dli_fname) {
                module = where.dli_fname;
                offset = (unsigned long)(uintptr_t)g_hangPc - (unsigned long)(uintptr_t)where.dli_fbase;
            }
            Emit("FRAME_HANG pc=0x%lx (%s+0x%lx) lr=0x%lx", g_hangPc, module, offset, g_hangLr);
            ReaDumpThreadAbi();
            ReaLogUnityJobs();
            if (g_unityBase) {
                DumpThreads();
            Emit("FRAME_HANG args x0=0x%lx (libunity+0x%lx) x1=0x%lx x2=0x%lx", g_hangArg0,
                     g_hangArg0 > g_unityBase ? g_hangArg0 - g_unityBase : 0, g_hangArg1, g_hangArg2);
            }
            /* Raw words first: filtering can hide the actual caller. */
            {
                unsigned long *raw = (unsigned long *)(uintptr_t)g_hangSp;
                for (int step = 0; step < 48; step++) {
                    unsigned long value = 0;
                    memcpy(&value, raw + step, sizeof(value));
                    if (value < 0x1000) continue;
                    Emit("HANG_RAW [%d] 0x%lx", step, value);
                }
            }

            /* Walk the waiting thread's own stack for return addresses inside libunity. */
            unsigned long *stack = (unsigned long *)(uintptr_t)g_hangSp;
            int reported = 0;
            for (int step = 0; step < 256 && reported < 10; step++) {
                unsigned long value = 0;
                memcpy(&value, stack + step, sizeof(value));
                if (!g_unityBase) continue;
                unsigned long guess = value - g_unityBase;
                if (value < g_unityBase || guess < 0x3fed90UL || guess >= 0x12ef5f4UL) continue;
                Emit("FRAME_STACK [%d] libunity+0x%lx", step, guess);
                reported++;
            }
            g_hangReported = true;
        }
    }
    return nullptr;
}

/* Render request handed to Unity's own thread. */
static volatile int g_renderRequested = 0;
static volatile int g_renderFinished = 0;
static void *g_renderTarget = nullptr;
static void *g_renderEnv = nullptr;
static void *g_renderPlayer = nullptr;

static void RenderOnUnityThreadHandler(int signalNumber)
{
    (void)signalNumber;
    if (!g_renderRequested || !g_renderTarget) return;
    g_renderRequested = 0;
    using RenderFn = unsigned char (*)(void *, void *);
    ((RenderFn)g_renderTarget)(g_renderEnv, g_renderPlayer);
    g_renderFinished = 1;
}

/* Finds the tid of a thread by its comm name. */
static int FindThreadByName(const char *wanted)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return -1;
    int found = -1;
    while (struct dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        char path[128];
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        FILE *comm = fopen(path, "r");
        if (!comm) continue;
        char name[64] = "";
        if (fgets(name, sizeof(name), comm)) {
            size_t length = strlen(name);
            if (length && name[length - 1] == '\n') name[length - 1] = '\0';
            if (strcmp(name, wanted) == 0) found = atoi(entry->d_name);
        }
        fclose(comm);
        if (found > 0) break;
    }
    closedir(dir);
    return found;
}

/* Lists thread names and their run states, to see which thread is expected to signal. */
static void DumpThreads(void)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return;
    int shown = 0;
    while (struct dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        char path[128];
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        FILE *comm = fopen(path, "r");
        char name[64] = "?";
        if (comm) {
            if (fgets(name, sizeof(name), comm)) {
                size_t length = strlen(name);
                if (length && name[length - 1] == '\n') name[length - 1] = '\0';
            }
            fclose(comm);
        }
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
        FILE *statFile = fopen(path, "r");
        char state = '?';
        if (statFile) {
            char line[512];
            if (fgets(line, sizeof(line), statFile)) {
                char *close = strrchr(line, ')');
                if (close && close[1] == ' ') state = close[2];
            }
            fclose(statFile);
        }
        if (shown < 80) {
            snprintf(path,sizeof(path),"/proc/self/task/%s/status",entry->d_name);
            if (FILE *status=fopen(path,"r")) {
                char line[256];
                while (fgets(line,sizeof(line),status))
                    if (!strncmp(line,"SigBlk:",7) || !strncmp(line,"SigPnd:",7)) {
                        line[strcspn(line,"\r\n")]=0;
                        Emit("THREAD_SIGNAL tid=%s %s",entry->d_name,line);
                    }
                fclose(status);
            }
            /* The syscall a thread is blocked in names the primitive it waits on. */
            char syscallText[96] = "";
            snprintf(path, sizeof(path), "/proc/self/task/%s/syscall", entry->d_name);
            FILE *syscallFile = fopen(path, "r");
            if (syscallFile) {
                if (fgets(syscallText, sizeof(syscallText), syscallFile)) {
                    size_t length = strlen(syscallText);
                    if (length && syscallText[length - 1] == '\n') syscallText[length - 1] = '\0';
                }
                fclose(syscallFile);
            }
            Emit("HANG_THREAD %s state=%c syscall=[%s]", name, state, syscallText);
            shown++;
        }
    }
    closedir(dir);
}

static void StartHangWatchdog(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = HangSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, nullptr);
    pthread_t watchdog;
    if (pthread_create(&watchdog, nullptr, HangWatchdog, nullptr) == 0) pthread_detach(watchdog);
}

/* The graphics call is run here so a hang cannot block the UI thread. */
static void *GfxWorker(void *raw)
{
    TracedCall *call = (TracedCall *)raw;
    Emit("GFX worker started (in-process call)");
    using GfxFn = void (*)(void *, void *, int, void *);
    ((GfxFn)call->target)(call->env, call->receiver, (int)(intptr_t)call->arg, call->objectArg);
    Emit("GFX worker: graphics initialisation returned (%d framework calls)", g_traceJniCount);
    Emit("GFX worker: threads after graphics init = %d", CountThreads());
    g_traceJni = false;

    if (g_renderForWorker) {
        using RenderFn = unsigned char (*)(void *, void *);
        Emit("GFX worker: starting render frames");
        g_frameThread = pthread_self();
        g_frameThreadValid = true;
        StartHangWatchdog();
        for (int frame = 0; frame < 60; frame++) {
            g_frameInProgress = true;
            g_hangReported = false;
            unsigned char drawn = ((RenderFn)g_renderForWorker)(call->env, call->receiver);
            g_frameInProgress = false;
            Emit("nativeRender #%d -> %u", frame, (unsigned)drawn);
            if (!drawn) break;
        }
        Emit("GFX worker: render loop finished");
    }
    Emit("GFX worker finished");
    return nullptr;
}

bool RunTraced(TracedCall *call)
{
    pid_t child = fork();
    if (child == 0) {
        /* The tracer must see the signal, so do not let inherited handlers swallow it. */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        signal(SIGILL, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
            _exit(2);
        }
        raise(SIGSTOP);
        ChildBody(call);
        _exit(0);
    }
    if (child < 0) {
        Emit("TRACE fork failed");
        return true;
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        Emit("TRACE initial wait failed");
        return true;
    }
    if (!WIFSTOPPED(status)) {
        Emit("TRACE child did not stop for attach (status 0x%x)", status);
        return true;
    }
    /*
     * Watch the device-creation entry. Whether it is reached decides if nativeRecreateGfxState actually
     * does its work or returns through one of the two guards.
     */
    unsigned long workAddress = g_unityBase ? g_unityBase + 0x73cb14UL : 0;    unsigned long savedWorkInstruction = 0;
    bool workPlanted = false;
    bool workReached = false;
    if (workAddress) {
        errno = 0;
        long original = ptrace(PTRACE_PEEKDATA, child, (void *)workAddress, 0);
        if (original != -1 || errno == 0) {
            savedWorkInstruction = (unsigned long)original;
            workPlanted = ptrace(PTRACE_POKEDATA, child, (void *)workAddress, (void *)0xD4200000UL) == 0;
        }
    }

    ptrace(PTRACE_CONT, child, 0, 0);

    /* Timed wait: a blocked render call is interrupted so its location can be reported. */
    for (int waited = 0; waited < 600; waited++) {
        pid_t probePid = waitpid(child, &status, WNOHANG);
        if (probePid == child) break;
        usleep(100000);
        if (waited == 100) {
            ptrace(PTRACE_INTERRUPT, child, 0, 0);
        }
    }
    if (!WIFSTOPPED(status)) {
        ptrace(PTRACE_INTERRUPT, child, 0, 0);
        waitpid(child, &status, 0);
        if (WIFSTOPPED(status)) {
            struct user_pt_regs stuckRegs;
            if (GetChildRegs(child, &stuckRegs)) {
                Dl_info info;
                memset(&info, 0, sizeof(info));
                const char *where = "?";
                unsigned long whereOffset = 0;
                if (dladdr((void *)(uintptr_t)stuckRegs.pc, &info) && info.dli_fname) {
                    where = info.dli_fname;
                    whereOffset = (unsigned long)(uintptr_t)stuckRegs.pc -
                                  (unsigned long)(uintptr_t)info.dli_fbase;
                }
                Emit("STUCK pc=0x%lx (%s+0x%lx) lr=0x%lx", (unsigned long)stuckRegs.pc, where,
                     whereOffset, (unsigned long)stuckRegs.regs[30]);
            }
        }
        Emit("STUCK render call did not complete within 60s");
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return true;
    }

    for (;;) {
        int waitResult = WaitChildOrInterrupt(child, &status);
        if (waitResult < 0) {
            Emit("TRACE wait failed");
            return true;
        }
        if (waitResult == 1) {
            /* Our own interrupt fired: the call has hung, so report where it is. */
            struct user_pt_regs hungRegs;
            if (GetChildRegs(child, &hungRegs)) {
                Dl_info hungInfo;
                memset(&hungInfo, 0, sizeof(hungInfo));
                const char *where = "?";
                unsigned long whereOffset = 0;
                if (dladdr((void *)(uintptr_t)hungRegs.pc, &hungInfo) && hungInfo.dli_fname) {
                    where = hungInfo.dli_fname;
                    whereOffset = (unsigned long)(uintptr_t)hungRegs.pc -
                                  (unsigned long)(uintptr_t)hungInfo.dli_fbase;
                }
                Emit("STUCK pc=0x%lx (%s+0x%lx) lr=0x%lx sp=0x%lx", (unsigned long)hungRegs.pc, where,
                     whereOffset, (unsigned long)hungRegs.regs[30], (unsigned long)hungRegs.sp);
            }
            Emit("STUCK render call made no progress for 60s");
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            return true;
        }
        if (WIFEXITED(status)) {
            Emit("TRACE child exited normally with %d (engine call completed)", WEXITSTATUS(status));
            return false;
        }
        if (!WIFSTOPPED(status)) {
            Emit("TRACE child ended unexpectedly (status 0x%x)", status);
            return true;
        }

        int signo = WSTOPSIG(status);
        if (signo == SIGTRAP) {
            if (workPlanted && !workReached) {
                struct user_pt_regs trapRegs;
                if (GetChildRegs(child, &trapRegs) &&
                    ((unsigned long)trapRegs.pc == workAddress ||
                     (unsigned long)trapRegs.pc == workAddress + 4)) {
                    workReached = true;
                    Emit("GFX_WORK breakpoint hit at 0x%lx", workAddress);
                    ptrace(PTRACE_POKEDATA, child, (void *)workAddress, (void *)savedWorkInstruction);
                    ptrace(PTRACE_CONT, child, 0, 0);
                    continue;
                }
            }
            /* Reached only for genuine traps; keep going. */
            ptrace(PTRACE_CONT, child, 0, 0);
            continue;
        }

        struct user_pt_regs regs;
        struct iovec io;
        io.iov_base = &regs;
        io.iov_len = sizeof(regs);
        if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &io) == 0) {
            Dl_info where;
            memset(&where, 0, sizeof(where));
            const char *module = "?";
            unsigned long offset = 0;
            if (regs.pc && dladdr((void *)regs.pc, &where) && where.dli_fname) {
                module = where.dli_fname;
                offset = regs.pc - (unsigned long)(uintptr_t)where.dli_fbase;
            }
            /* Registry state inside the child, read through ptrace at the fault. */
            if (g_unityBase) {
                unsigned long registry[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                for (int i = 0; i < 8; i++) {
                    errno = 0;
                    long word = ptrace(PTRACE_PEEKDATA, child,
                                       (void *)(uintptr_t)(g_unityBase + 0x1379148UL + i * 8), 0);
                    if (word == -1 && errno != 0) break;
                    registry[i] = (unsigned long)word;
                }
                Emit("CHILD_REGISTRY %lx %lx %lx %lx %lx %lx %lx %lx", registry[0], registry[1],
                     registry[2], registry[3], registry[4], registry[5], registry[6], registry[7]);
            }

            /* Reconstruct the chain: return addresses on the child's stack. */
            if (g_unityBase) {
                int reported = 0;
                for (int step = 0; step < 400 && reported < 14; step++) {
                    unsigned long address = (unsigned long)regs.sp + (unsigned long)step * 8;
                    errno = 0;
                    long word = ptrace(PTRACE_PEEKDATA, child, (void *)address, 0);
                    if (word == -1 && errno != 0) continue;
                    unsigned long value = (unsigned long)word;
                    /* .text only: anything past it is data, not a return address. */
                    unsigned long offset = value - g_unityBase;
                    if (value < g_unityBase || offset < 0x3fed90UL || offset >= 0x12ef5f4UL) continue;
                    Emit("CHILD_FRAME [%d] libunity+0x%lx", step, value - g_unityBase);
                    reported++;
                }
            }

            Dl_info lrWhere;
            memset(&lrWhere, 0, sizeof(lrWhere));
            const char *lrModuleName = "?";
            unsigned long lrOffset = 0;
            if (regs.regs[30] && dladdr((void *)(uintptr_t)regs.regs[30], &lrWhere) &&
                lrWhere.dli_fname) {
                lrModuleName = lrWhere.dli_fname;
                lrOffset = (unsigned long)(uintptr_t)regs.regs[30] -
                           (unsigned long)(uintptr_t)lrWhere.dli_fbase;
            }
            Emit("TRACE fault signal=%d pc=0x%llx (%s+0x%lx) lr=0x%llx (%s+0x%lx) sp=0x%llx",
                 signo, (unsigned long long)regs.pc, module, offset,
                 (unsigned long long)regs.regs[30], lrModuleName, lrOffset,
                 (unsigned long long)regs.sp);

            /* Full register file: the null target register identifies the kind of branch. */
            for (int block = 0; block < 3; block++) {
                char line[512];
                int offset = 0;
                offset += snprintf(line + offset, sizeof(line) - offset, "TRACE regs%d", block);
                for (int i = block * 10; i < block * 10 + 10 && i < 31; i++) {
                    offset += snprintf(line + offset, sizeof(line) - offset,
                                       " x%d=%llx", i, (unsigned long long)regs.regs[i]);
                }
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
            }
            {
                char line[256];
                snprintf(line, sizeof(line), "TRACE regs3 x30=%llx sp=%llx pc=%llx pstate=%llx",
                         (unsigned long long)regs.regs[30], (unsigned long long)regs.sp,
                         (unsigned long long)regs.pc, (unsigned long long)regs.pstate);
                OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
            }
            /* Walk the real stack: saved return addresses identify the returning frame. */
            {
                int reported = 0;
                for (int distance = 0; distance < 512 && reported < 16; distance++) {
                    int steps[2] = { distance, -distance };
                    for (int s = 0; s < 2 && reported < 16; s++) {
                        int step = steps[s];
                        if (distance == 0 && s == 1) continue;
                        unsigned long address = (unsigned long)regs.sp + (unsigned long)step * 8;
                        unsigned long value = (unsigned long)ptrace(PTRACE_PEEKDATA, child, (void *)address, 0);
                        if (!value) continue;
                        Dl_info info;
                        memset(&info, 0, sizeof(info));
                        if (!dladdr((void *)value, &info) || !info.dli_fname) continue;
                        char line[256];
                        snprintf(line, sizeof(line), "TRACE near[%d] 0x%lx -> %s+0x%lx", step, value,
                                 info.dli_fname, value - (unsigned long)(uintptr_t)info.dli_fbase);
                        OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG, "%{public}s", line);
                        reported++;
                    }
                }
                if (reported == 0) {
                    OH_LOG_Print(LOG_APP, LOG_FATAL, LOG_DOMAIN, LOG_TAG,
                                 "%{public}s", "TRACE stack: no engine frames found");
                }
            }
        } else {
            Emit("TRACE GETREGSET failed");
        }

        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return true;
    }
}

} // namespace

bool g_traceGfx = false;
void *g_il2cppHandle = nullptr;
/* Set to true to skip the slot back-fill entirely (A/B experiment). */
static bool g_disableSlotFill = false;

/* One entry per libunity slot that holds an il2cpp entry point resolved through Unity's module table. */
struct Il2CppSlot {
    unsigned long offset;
    const char *name;
    bool indirect;   /* true: slot -> struct -> function; false: slot holds the function */
};

#include "il2cpp_slots.inc"

/*
 * Reports whether an address falls inside a writable PT_LOAD segment of the given image.
 *
 * Segments are page aligned and may be separated by gaps, so a map is walked rather than assuming one
 * contiguous range. p_flags bit 1 is PF_W.
 */
static bool IsWritableInImage(unsigned long base, unsigned long address)
{
    unsigned char *elf = (unsigned char *)base;
    unsigned long long phoff = 0;
    unsigned short phentsize = 0, phnum = 0;
    memcpy(&phoff, elf + 0x20, 8);
    memcpy(&phentsize, elf + 0x36, 2);
    memcpy(&phnum, elf + 0x38, 2);
    if (!phoff || !phentsize || !phnum) return false;

    const unsigned long pageSize = 4096;
    for (int i = 0; i < phnum; i++) {
        unsigned char *header = elf + phoff + (unsigned long long)i * phentsize;
        unsigned int type = 0, flags = 0;
        unsigned long long vaddr = 0, memsz = 0;
        memcpy(&type, header, 4);
        memcpy(&flags, header + 4, 4);
        memcpy(&vaddr, header + 0x10, 8);
        memcpy(&memsz, header + 0x28, 8);
        if (type != 1 /* PT_LOAD */) continue;
        if ((flags & 0x2) == 0) continue;   /* not writable */

        unsigned long start = (unsigned long)(base + vaddr) & ~(pageSize - 1);
        unsigned long end = ((unsigned long)(base + vaddr + memsz) + pageSize - 1) & ~(pageSize - 1);
        if (address >= start && address < end) return true;
    }
    return false;
}

/* Fills slots whose function pointer is still null. Returns how many were repaired. */
/* Samples the subsystem registry so the step that fills it can be identified. */
static void SampleRegistry(const char *stage)
{
    if (!g_unityBase) return;
    unsigned long *table = (unsigned long *)(uintptr_t)(g_unityBase + 0x1379148UL);
    Emit("REGSAMPLE %s: %lx %lx %lx %lx %lx %lx", stage, table[0], table[1], table[2], table[3], table[4],
         table[5]);
}

static int FillIl2CppSlots(unsigned long unityBase)
{
    if (!unityBase) return 0;
    if (g_disableSlotFill) {
        Emit("IL2CPP_SLOTS disabled (A/B experiment)");
        return 0;
    }
    int filled = 0;
    int missing = 0;
    int indirectCount = 0;
    /* Filled lazily by IsWritableInImage; kept for the summary log. */
    unsigned long imageEnd = 0;
    (void)imageEnd;

    for (const auto &slot : kIl2cppSlots) {
        unsigned long *cell = (unsigned long *)(uintptr_t)(unityBase + slot.offset);

        void *symbol = g_il2cppHandle ? dlsym(g_il2cppHandle, slot.name) : nullptr;
        if (!symbol) {
            missing++;
            continue;
        }
        /* This one is always ours: libil2cpp cannot read the metadata out of the HAP. */
        bool forceInstall = (strcmp(slot.name, "il2cpp_set_data_dir") == 0) ||
                            (strcmp(slot.name, "il2cpp_class_from_name") == 0) ||
                            (strcmp(slot.name, "il2cpp_class_get_method_from_name") == 0);
        if (strcmp(slot.name, "il2cpp_set_data_dir") == 0) {
            ReaSetRealIl2cppDataDir(symbol);
            symbol = (void *)&ReaIl2cppSetDataDir;
        }

        /* Report scripting-runtime initialisation and managed-method resolution. */
        if (strcmp(slot.name, "il2cpp_init") == 0) {
            ReaSetRealIl2cppInit(symbol, dlsym(g_il2cppHandle, "il2cpp_domain_get"));
            symbol = (void *)&rea_il2cpp_init;
        } else if (strcmp(slot.name, "il2cpp_domain_get") == 0) {
            symbol = (void *)&rea_il2cpp_domain_get;
        } else if (strcmp(slot.name, "il2cpp_class_from_name") == 0 ||
                   strcmp(slot.name, "il2cpp_class_get_method_from_name") == 0) {
            ReaSetRealIl2cppLookups(dlsym(g_il2cppHandle, "il2cpp_class_from_name"),
                                    dlsym(g_il2cppHandle, "il2cpp_class_get_method_from_name"));
            symbol = strcmp(slot.name, "il2cpp_class_from_name") == 0
                         ? (void *)&rea_il2cpp_class_from_name
                         : (void *)&rea_il2cpp_class_get_method_from_name;
        }

        if (slot.indirect) {
            /* Slot holds a pointer to a struct whose first word is the resolved function. */
            unsigned long object = *cell;
            if (!object) {
                missing++;
                continue;
            }
            if (!IsWritableInImage(unityBase, object)) {
                Emit("IL2CPP_SLOTS skip %s: target 0x%lx is not writable image memory", slot.name,
                     object);
                missing++;
                continue;
            }
            unsigned long *function = (unsigned long *)(uintptr_t)object;
            if (*function && !forceInstall) continue;
            *function = (unsigned long)(uintptr_t)symbol;
            indirectCount++;
        } else {
            /* Slot holds the function pointer itself; only fill it when it is still empty. */
            if (*cell && !forceInstall) continue;
            /* Relro pages are read-only after relocation, so unlock the page before storing. */
            {
                const unsigned long pageSize = 4096;
                void *page = (void *)((unsigned long)(uintptr_t)cell & ~(pageSize - 1));
                if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0) {
                    Emit("IL2CPP_SLOTS skip %s: slot page is not writable", slot.name);
                    missing++;
                    continue;
                }
            }
            *cell = (unsigned long)(uintptr_t)symbol;
        }

        if (filled < 4) {
            Emit("IL2CPP_SLOTS sample %s: slot=0x%lx indirect=%d -> %p", slot.name,
                 (unsigned long)(uintptr_t)cell, slot.indirect ? 1 : 0, symbol);
        }
        filled++;
    }

    Emit("IL2CPP_SLOTS filled=%d (indirect=%d) unresolved=%d total=%d", filled, indirectCount, missing,
         (int)(sizeof(kIl2cppSlots) / sizeof(kIl2cppSlots[0])));
    return filled;
}

/*
 * Full bootstrap: libmain's JNI_OnLoad, then the native it registers
 * - NativeLoader.load(path) - which is how Unity's Java layer loads libunity.so and lets the
 * engine register its own native surface.
 */
namespace {
std::mutex g_unityJobsMutex;
std::vector<FakeObject *> g_unityJobs;
struct TouchInput {int action,id;float x,y;int64_t sourceNs,entryNs;};
static int64_t MonotonicNs(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
static std::atomic<bool> g_inputReady{false};
static FakeObject *g_inputPlayer=nullptr;
static void *g_inputInject=nullptr;
static std::mutex g_touchStateMutex;
static std::map<int,TouchPoint> g_activeTouches;
static int64_t g_touchDownTime=0;
struct TouchMetrics {
    uint64_t count=0,validSource=0,rejected=0,lastSequence=0;
    int64_t lastSource=0,lastEntry=0,lastDone=0,sourceSum=0,sourceMax=0,injectSum=0,injectMax=0;
    int active=0;
};
static std::mutex g_touchMetricsMutex;
static TouchMetrics g_touchMetrics;
static TouchMetrics TouchSnapshot(){std::lock_guard<std::mutex> lock(g_touchMetricsMutex);return g_touchMetrics;}
static void RecordTouchFrame(const TouchMetrics &before,int64_t frameDone)
{
    static uint64_t observed=0,lastPrinted=0;
    static int64_t lastPrintNs=0;
    static double samples[256]{};
    static unsigned sampleCount=0,nextSample=0;
    if(before.lastSequence && before.lastSequence!=observed){
        observed=before.lastSequence;
        int64_t from=before.lastSource>0?before.lastSource:before.lastEntry;
        if(frameDone>=from){samples[nextSample++%256]=(frameDone-from)/1e6;sampleCount=std::min(sampleCount+1,256U);}
    }
    if(before.count==lastPrinted || frameDone-lastPrintNs<2000000000LL)return;
    double sorted[256];std::copy(samples,samples+sampleCount,sorted);std::sort(sorted,sorted+sampleCount);
    double p50=sampleCount?sorted[(sampleCount-1)*50/100]:-1,p95=sampleCount?sorted[(sampleCount-1)*95/100]:-1;
    Emit("TOUCH_LATENCY count=%llu sourceValid=%llu source_to_app_avg_ms=%.3f source_to_app_max_ms=%.3f inject_avg_ms=%.3f inject_max_ms=%.3f next_render_p50_ms=%.3f next_render_p95_ms=%.3f active=%d rejected=%llu",
        (unsigned long long)before.count,(unsigned long long)before.validSource,
        before.validSource?before.sourceSum/1e6/before.validSource:-1,before.sourceMax/1e6,
        before.count?before.injectSum/1e6/before.count:0,before.injectMax/1e6,p50,p95,before.active,(unsigned long long)before.rejected);
    lastPrinted=before.count;lastPrintNs=frameDone;
}
static void SubmitTouch(const TouchInput &input,bool cancelForPause=false)
{
    std::lock_guard<std::mutex> lock(g_touchStateMutex);
    // Recheck after taking the lock: a UI event may have passed the outer
    // ready check immediately before the owner thread began pausing.
    if(!cancelForPause&&!g_inputReady.load(std::memory_order_acquire))return;
    bool existed=g_activeTouches.count(input.id)!=0;
    if(input.action!=0 && input.action!=3 && !existed)return;
    if(input.action==3 && g_activeTouches.empty())return;
    int64_t time=(input.sourceNs>0?input.sourceNs:input.entryNs)/1000000;
    if(input.action==0 && g_activeTouches.empty())g_touchDownTime=time;
    if(input.action!=3 || existed)g_activeTouches[input.id]={input.id,input.x,input.y};
    auto *event=NewObject("android/view/MotionEvent");
    int changed=0;float scaleX=1,scaleY=1;ReaGetTouchBufferScale(&scaleX,&scaleY);
    for(const auto &point:g_activeTouches){
        if(point.first==input.id)changed=event->touchPoints.size();
        event->touchPoints.push_back({point.second.id,point.second.x*scaleX,point.second.y*scaleY});
    }
    int action=input.action;
    if(action==0 && g_activeTouches.size()>1)action=5|(changed<<8);
    if(action==1 && g_activeTouches.size()>1)action=6|(changed<<8);
    event->ints["action"]=action;event->ints["getEventTime"]=time;event->ints["getDownTime"]=g_touchDownTime;
    unsigned char accepted=((unsigned char (*)(void *,void *,void *,int))g_inputInject)(&g_env,g_inputPlayer,event,0);
    int64_t done=MonotonicNs();
    if(input.action==1)g_activeTouches.erase(input.id);
    if(input.action==3)g_activeTouches.clear();
    std::lock_guard<std::mutex> metricsLock(g_touchMetricsMutex);
    auto &m=g_touchMetrics;++m.count;++m.lastSequence;if(!accepted)++m.rejected;
    m.lastSource=input.sourceNs;m.lastEntry=input.entryNs;m.lastDone=done;m.active=(int)g_activeTouches.size();
    if(input.sourceNs>0){int64_t age=input.entryNs-input.sourceNs;++m.validSource;m.sourceSum+=age;m.sourceMax=std::max(m.sourceMax,age);}
    int64_t duration=done-input.entryNs;m.injectSum+=duration;m.injectMax=std::max(m.injectMax,duration);
}
static void QueueUnityJob(FakeObject *job,bool inlineOnOwner)
{
    // Exact original UnityPlayer.java semantics: work submitted by its own
    // Unity thread runs immediately, other callers enqueue for that thread.
    // Deferring owner work can deadlock a native operation waiting inside Render.
    if(inlineOnOwner&&g_frameThreadValid&&pthread_equal(pthread_self(),g_frameThread)){
        Emit("UNITY_JOB_INLINE class=%s",job?job->cls.c_str():"null");
        InvokeJavaProxy(job,"run",nullptr);return;
    }
    std::lock_guard<std::mutex> lock(g_unityJobsMutex);
    g_unityJobs.push_back(job);
    if(g_unityJobs.size()<=8)Emit("UNITY_JOB_ENQUEUE pending=%zu",g_unityJobs.size());
}
extern "C" void ReaLogUnityJobs(){
    std::lock_guard<std::mutex> lock(g_unityJobsMutex);
    Emit("UNITY_JOB_STATE pending=%zu",g_unityJobs.size());
}
struct UnityHostStart {
    FakeObject *player;
    FakeObject *surface;
    void *gfx;
    void *render;
    void *surfaceChanged;
    void *focus;
    void *resume;
    void *inject;
    void *pause;
};
static void *UnityHostBody(void *raw)
{
    std::unique_ptr<UnityHostStart> start(static_cast<UnityHostStart *>(raw));
    pthread_setname_np(pthread_self(), "ReaUnityMain");
    Emit("UNITY_HOST started tid=%d initJni kept on UI thread",(int)syscall(SYS_gettid));
    g_frameThread=pthread_self();g_frameThreadValid=true;
    StartFaultWatchdog();StartHangWatchdog();
    bool running=false,everResumed=false;
    uint64_t frame=0,seenPauseEpoch=0,boundEpoch=0;
    std::shared_ptr<ReaNativeSurface> bound;
    auto focus=[&](bool value){if(start->focus)((void (*)(void *,void *,unsigned char))start->focus)(&g_env,start->player,value?1:0);};
    auto resume=[&]{if(start->resume)((void (*)(void *,void *))start->resume)(&g_env,start->player);};
    auto gfx=[&](FakeObject *surface){((void (*)(void *,void *,int,void *))start->gfx)(&g_env,start->player,0,surface);};
    auto surfaceChanged=[&]{if(start->surfaceChanged)((void (*)(void *,void *))start->surfaceChanged)(&g_env,start->player);};
    for (;;) {
        auto state=ReaReadLifecycle();
        if(running&&(!state.foreground||!state.surface||state.pauseEpoch!=seenPauseEpoch||state.surfaceEpoch!=boundEpoch)) {
            g_inputReady.store(false,std::memory_order_release);
            SubmitTouch({3,0,0,0,0,MonotonicNs()},true);
            focus(false);
            Emit("LIFECYCLE pause begin frame=%llu epoch=%llu tid=%d",(unsigned long long)frame,(unsigned long long)state.pauseEpoch,(int)syscall(SYS_gettid));
            int64_t before=MonotonicNs();
            unsigned char quit=start->pause?((unsigned char (*)(void *,void *))start->pause)(&g_env,start->player):0;
            Emit("LIFECYCLE pause done quit=%u ms=%.3f",(unsigned)quit,(MonotonicNs()-before)/1e6);
            running=false;
            if(quit){ReaRequestPlayerExit("nativePause(true)");break;}
        }
        seenPauseEpoch=state.pauseEpoch;
        if(bound&&(!state.surface||state.surfaceEpoch!=boundEpoch)) {
            Emit("LIFECYCLE detach begin epoch=%llu",(unsigned long long)boundEpoch);
            gfx(nullptr);
            {std::lock_guard<std::mutex> lock(g_videoJniMutex);start->surface->nativeSurface.reset();}
            bound.reset();
            Emit("LIFECYCLE detach done");
        }
        if(!running&&state.foreground&&state.surface) {
            // Preserve the proven cold-start order. Subsequent surface binding
            // precedes resume/focus, as in the original UnityPlayer Java queue.
            bool first=!everResumed;
            if(first){focus(true);resume();}
            if(!bound) {
                bound=state.surface;boundEpoch=state.surfaceEpoch;
                start->surface=NewObject("android/view/Surface");
                {std::lock_guard<std::mutex> lock(g_videoJniMutex);start->surface->nativeSurface=bound;}
                gfx(start->surface);surfaceChanged();
            }
            if(!first){resume();focus(true);}
            everResumed=true;running=true;
            g_inputReady.store(frame>=4&&start->inject,std::memory_order_release);
            Emit("LIFECYCLE resume done frame=%llu surface=%llu tid=%d",(unsigned long long)frame,(unsigned long long)boundEpoch,(int)syscall(SYS_gettid));
        } else if(running&&bound&&state.surface&&(bound->width!=state.surface->width||bound->height!=state.surface->height)) {
            bound=state.surface;surfaceChanged();
        }
        // Original T0.java services GL jobs even while rendering is paused.
        // Calls into Unity never hold the lifecycle mutex or block ArkUI.
        std::vector<FakeObject *> jobs;
        {std::lock_guard<std::mutex> lock(g_unityJobsMutex);jobs.swap(g_unityJobs);}
        for (auto *job:jobs) InvokeJavaProxy(job,"run",nullptr);
        ReaSdkDrainReplies();
        DrainKeyboard(start->player);
        DrainNativeDialogs();
        if(!running){ReaWaitLifecycle(state.serial,20);continue;}
        auto latest=ReaReadLifecycle();
        if(!latest.foreground||!latest.surface||latest.pauseEpoch!=seenPauseEpoch||latest.surfaceEpoch!=boundEpoch)continue;
        TouchMetrics inputBeforeFrame=TouchSnapshot();
        auto begin=std::chrono::steady_clock::now();
        g_frameSerial=frame;g_frameInProgress=true;g_hangReported=false;
        unsigned char more=((unsigned char (*)(void *,void *))start->render)(&g_env,start->player);
        RecordTouchFrame(inputBeforeFrame,MonotonicNs());
        if(frame>=1200&&frame%1200==0)ReaLogEntryState();
        if(frame==3 && start->inject){g_inputPlayer=start->player;g_inputInject=start->inject;g_inputReady.store(true,std::memory_order_release);Emit("TOUCH_DIRECT ready; UI-thread JNI injection, original event timestamps");}
        g_frameInProgress=false;
        if (frame<5 || frame%120==0 || !more) Emit("UNITY_HOST nativeRender frame=%llu result=%u tid=%d",(unsigned long long)frame,(unsigned)more,(int)syscall(SYS_gettid));
        if (!more) {
            // Original UnityPlayer T0.java: nativeRender(false) calls finish().
            ReaRequestPlayerExit("nativeRender(false)");break;
        }
        // The original engine/Choreographer owns its frame limit. Do not add a second 16ms gate.
        usleep(1000);
        ++frame;
    }
    g_inputReady.store(false,std::memory_order_release);
    g_frameThreadValid=false;
    Emit("UNITY_HOST render loop ended");
    return nullptr;
}
}

extern "C" void ReaQueueTouch(int action,int id,double x,double y,double sourceTimestamp)
{
    if(action<0 || action>3 || id<0 || !std::isfinite(x) || !std::isfinite(y) || !g_inputReady.load(std::memory_order_acquire))return;
    int64_t entry=MonotonicNs(),source=0;
    if(std::isfinite(sourceTimestamp) && sourceTimestamp>0 && sourceTimestamp<9e18){
        int64_t stamp=(int64_t)sourceTimestamp,age=entry-stamp;
        if(age>=0 && age<60000000000LL)source=stamp;
        else {
            timespec boot;clock_gettime(CLOCK_BOOTTIME,&boot);
            int64_t bootAge=(int64_t)boot.tv_sec*1000000000LL+boot.tv_nsec-stamp;
            if(bootAge>=0 && bootAge<60000000000LL)source=entry-bootAge;
        }
    }
    SubmitTouch({action,id,(float)x,(float)y,source,entry});
}

extern "C" const char *ReaUnityProbe()
{
    InstallFaultHandler();
    g_log.clear();
    g_slotsHit.clear();
    g_registered.clear();
    g_vm.functions = g_vmFunctions;
    Emit("=== unity bootstrap probe ===");

    void *main = dlopen("libmain.so", RTLD_NOW | RTLD_GLOBAL);
    if (!main) {
        const char *error = dlerror();
        Emit("LOAD_FAIL libmain.so: %s", error ? error : "?");
        return g_log.c_str();
    }
    using JniOnLoadFn = int (*)(void *, void *);
    JniOnLoadFn onLoad = (JniOnLoadFn)dlsym(main, "JNI_OnLoad");
    if (!onLoad) {
        Emit("SYM_MISSING libmain JNI_OnLoad");
        return g_log.c_str();
    }
    int version = onLoad(&g_vm, nullptr);
    Emit("libmain JNI_OnLoad -> 0x%x (natives now %zu)", version, g_registered.size());

    /*
     * Patch libmain's loader slots before anything is loaded: the libraries are opened inside
     * NativeLoader.load, so patching afterwards would miss the only interesting calls.
     */
    {
        Dl_info mainInfo;
        memset(&mainInfo, 0, sizeof(mainInfo));
        if (dladdr((void *)onLoad, &mainInfo) && mainInfo.dli_fbase) {
            int mainPatched = ReaPatchPthreadGOT(mainInfo.dli_fbase);
            Emit("libmain GOT slots redirected early: %d", mainPatched);
        }
    }

    void *loadFn = nullptr;
    for (const auto &entry : g_registered) {
        if (entry.name == "load") {
            loadFn = entry.fnPtr;
            break;
        }
    }
    if (!loadFn) {
        Emit("NativeLoader.load not registered; cannot continue");
        return g_log.c_str();
    }

    /*
     * NativeLoader.load() takes the native library DIRECTORY, exactly as Android's
     * ApplicationInfo.nativeLibraryDir does. Passing the .so path instead makes the engine probe
     * "<path>/libil2cpp.so" and fail with errno=20, so keep this a directory.
     */
    const char *engineDir = "/data/storage/el1/bundle/libs/arm64";
    FakeObject *pathString = NewObject("java/lang/String");
    pathString->strs["value"] = engineDir;
    FakeObject *loaderClass = NewObject("java/lang/Class");
    loaderClass->strs["name"] = "com/unity3d/player/NativeLoader";

    using LoadFn = unsigned char (*)(void *, void *, void *);
    /*
     * Unity resolves its il2cpp entry points through its own module table. If libil2cpp lives in a
     * non-global scope, every one of those lookups returns null and the engine later calls through the
     * null pointers. Loading it globally first lets the lookups succeed on their own.
     */
    {
        /* Disabled to test whether preloading keeps Unity's module registry from seeing libil2cpp. */
        void *preload = nullptr;
        if (!preload) {
            Emit("il2cpp global preload failed: %s", dlerror());
        } else {
            Emit("il2cpp global preload ok, RTLD_DEFAULT lookup = %p",
                 dlsym(RTLD_DEFAULT, "il2cpp_register_log_callback"));
        }
    }

    LoadFn load = (LoadFn)loadFn;
    unsigned char ok = load(&g_env, loaderClass, pathString);
    Emit("NativeLoader.load -> %u %s", (unsigned)ok, ok ? "(engine loaded)" : "(engine refused)");

    /* Which lifecycle entry points does the engine actually expose? */
    {
        int shown = 0;
        for (const auto &entry : g_registered) {
            if (entry.className.find("UnityPlayer") == std::string::npos) continue;
            Emit("NATIVE %s%s", entry.name.c_str(), entry.signature.c_str());
            if (++shown >= 40) break;
        }
        Emit("NATIVE total registered=%zu shown=%d", g_registered.size(), shown);

        /* Offsets identify which lifecycle call reaches a given internal function. */
        {
            Dl_info self;
            memset(&self, 0, sizeof(self));
            unsigned long localBase = 0;
            if (dladdr((void *)onLoad, &self) && self.dli_fbase) {
                localBase = (unsigned long)(uintptr_t)self.dli_fbase;
            }
            if (localBase) {
                for (const auto &entry : g_registered) {
                    unsigned long offset = (unsigned long)(uintptr_t)entry.fnPtr - localBase;
                    if (offset > 0x2000000UL) continue;   /* not inside libunity */
                    Emit("NATIVEOFF 0x%lx %s", offset, entry.name.c_str());
                }
            }
        }
    }

    /* Inspect the PLT slot the fault pointed at: 0 means either lazy binding or a failed resolve. */
    {
        Dl_info unityInfo;
        memset(&unityInfo, 0, sizeof(unityInfo));
        if (dladdr((void *)loadFn, &unityInfo) && unityInfo.dli_fbase) {
            unsigned long base = (unsigned long)(uintptr_t)unityInfo.dli_fbase;
            const unsigned long slotOffset = 0x133e3b0;
            unsigned long slotValue = (unsigned long)(uintptr_t) * (void **)(base + slotOffset);
            Emit("libunity base=0x%lx got[0x%lx]=0x%lx", base, slotOffset, slotValue);

            /* Count zero slots across .got.plt to see whether binding is lazy for this library. */
            int zeroSlots = 0;
            int totalSlots = 0;
            for (unsigned long offset = 0x133e238; offset < 0x133e238 + 0xC60; offset += 8) {
                unsigned long value = (unsigned long)(uintptr_t) * (void **)(base + offset);
                totalSlots++;
                if (value == 0) zeroSlots++;
            }
            Emit("got.plt slots=%d zero=%d", totalSlots, zeroSlots);
        } else {
            Emit("could not resolve the libunity load base");
        }
    }
    Emit("registered natives total: %zu", g_registered.size());
    /* True libunity base comes from a native that is known to live in it. */
    {
        unsigned long unityBase = 0;
        for (const auto &entry : g_registered) {
            if (entry.name == "initJni") {
                unityBase = (unsigned long)(uintptr_t)entry.fnPtr - 0x7567d4UL;
            g_unityBase = unityBase;
                break;
            }
        }
        if (unityBase) {
            Emit("libunity true base=0x%lx", unityBase);
            int zeroSlots = 0;
            int reported = 0;
            for (unsigned long offset = 0x133e238; offset < 0x133e238 + 0xC60; offset += 8) {
                unsigned long value;
                memcpy(&value, (void *)(unityBase + offset), sizeof(value));
                if (value != 0) continue;
                zeroSlots++;
                if (reported < 12) {
                    Emit("ZERO_GOT offset=0x%lx", offset);
                    reported++;
                }
            }
            Emit("got.plt total=%d zero=%d", (0xC60 / 8), zeroSlots);
            /* The crash path uses these two slots; report what they actually hold. */
            unsigned long probeOffsets[3] = { 0x133e2a0, 0x133e2a8, 0x133e248 };
            for (unsigned long probeOffset : probeOffsets) {
                unsigned long value;
                memcpy(&value, (void *)(unityBase + probeOffset), sizeof(value));
                Dl_info slotInfo;
                memset(&slotInfo, 0, sizeof(slotInfo));
                if (value && dladdr((void *)value, &slotInfo) && slotInfo.dli_fname) {
                    Emit("GOTSLOT 0x%lx = 0x%lx -> %s+0x%lx", probeOffset, value, slotInfo.dli_fname,
                         value - (unsigned long)(uintptr_t)slotInfo.dli_fbase);
                } else {
                    Emit("GOTSLOT 0x%lx = 0x%lx (unattributed)", probeOffset, value);
                }
            }
            void *realGetSpecific = dlsym(RTLD_DEFAULT, "pthread_getspecific");
            Emit("dlsym pthread_getspecific = %p", realGetSpecific);
            void *realLock = dlsym(RTLD_DEFAULT, "pthread_mutex_lock");
            void *realUnlock = dlsym(RTLD_DEFAULT, "pthread_mutex_unlock");
            Emit("dlsym mutex_lock=%p mutex_unlock=%p", realLock, realUnlock);
        } else {
            Emit("could not derive the libunity base");
        }
    }

    /* Attribute the key entry points to a loaded module before calling them. */
    for (const auto &entry : g_registered) {
        if (entry.name != "initJni" && entry.name != "nativeRender" &&
            entry.name != "nativeRecreateGfxState") {
            continue;
        }
        Dl_info info;
        memset(&info, 0, sizeof(info));
        if (dladdr(entry.fnPtr, &info) && info.dli_fname) {
            Emit("OWNER %s -> %s+0x%lx", entry.name.c_str(), info.dli_fname,
                 (unsigned long)(uintptr_t)entry.fnPtr - (unsigned long)(uintptr_t)info.dli_fbase);
        } else {
            Emit("OWNER %s -> unresolved", entry.name.c_str());
        }
    }


    /*
     * Android's pthread_mutex_t is 4 bytes while HarmonyOS declares 40, so the engine's mutexes would
     * be written past their end by the platform implementation. Redirect this library's pthread
     * slots to the translating layer before anything initialises a mutex.
     */
    {
        unsigned long patchBase = 0;
        for (const auto &entry : g_registered) {
            if (entry.name == "initJni") {
                patchBase = (unsigned long)(uintptr_t)entry.fnPtr - 0x7567d4UL;
                break;
            }
        }
        if (patchBase) {
            int redirected = ReaPatchPthreadGOT((void *)patchBase);
            Emit("pthread GOT slots redirected: %d", redirected);

            /* libmain performs the actual library loading, so trace its loader slots too. */
            Dl_info mainInfo;
            memset(&mainInfo, 0, sizeof(mainInfo));
            using MainOnLoadFn = int (*)(void *, void *);
            MainOnLoadFn mainOnLoad = (MainOnLoadFn)dlsym(main, "JNI_OnLoad");
            if (mainOnLoad && dladdr((void *)mainOnLoad, &mainInfo) && mainInfo.dli_fbase) {
                int mainPatched = ReaPatchPthreadGOT(mainInfo.dli_fbase);
                Emit("libmain GOT slots redirected: %d", mainPatched);
            }
            /* The scripting runtime uses the same pthread/sem surface, so patch it too. */
            void *il2cppHandle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
            if (il2cppHandle) {
                Dl_info il2cppInfo;
                memset(&il2cppInfo, 0, sizeof(il2cppInfo));
                using InitFn = int (*)(const char *);
                InitFn il2cppInit = (InitFn)dlsym(il2cppHandle, "il2cpp_init");
                if (il2cppInit && dladdr((void *)il2cppInit, &il2cppInfo) && il2cppInfo.dli_fbase) {
                    g_il2cppHandle = il2cppHandle;
                    ReaConfigureExceptionTrace(il2cppHandle);
                    int il2cppPatched = ReaPatchPthreadGOT(il2cppInfo.dli_fbase);
                    Emit("il2cpp GOT slots redirected: %d", il2cppPatched);
                }
            }
        } else {
            Emit("could not derive the libunity base for pthread patching");
        }
    }

    /*
     * Cover the case where the engine initialises il2cpp during startup rather than during rendering:
     * prepare the metadata, resolve the entry-point table and point il2cpp at the prepared directory
     * now. Filling again before rendering is harmless and covers the other order.
     */
    if (g_unityBase) {
        /*
         * The hook is not installed: mprotect cannot restore execute permission on this platform, and
         * leaving the page writable makes libunity's own code unfetchable. No code page is touched.
         */
        Emit("il2cpp lookup hook skipped (W^X would corrupt the code page)");
        ReaPrepareIl2cppData();
        int earlyFilled = FillIl2CppSlots(g_unityBase);
        Emit("IL2CPP_SLOTS early fill: %d", earlyFilled);
        /* Any path without metadata is redirected to the prepared directory by the wrapper. */
        ReaIl2cppSetDataDir("/nonexistent");

        /*
         * The engine resolves il2cpp_init but does not call it before looking up managed methods, so those
         * lookups find nothing. Initialise the runtime here, after the data directory is in place.
         */
        {
            if (g_manualIl2cppInit) {
                using InitFn = int (*)(const char *);
                InitFn scriptingInit =
                    g_il2cppHandle ? (InitFn)dlsym(g_il2cppHandle, "il2cpp_init") : nullptr;
                if (scriptingInit) {
                    int rc = scriptingInit("Unity");
                    Emit("explicit il2cpp_init -> %d", rc);

                    /*
                     * If managed code never loaded, no C# startup runs and the engine's subsystems are never
                     * registered - which would make faking native objects a dead end. Count the assemblies
                     * that the runtime actually holds.
                     */
                    using DomainFn = void *(*)(void);
                    using AssembliesFn = const void **(*)(const void *, size_t *);
                    DomainFn domainGet =
                        g_il2cppHandle ? (DomainFn)dlsym(g_il2cppHandle, "il2cpp_domain_get") : nullptr;
                    AssembliesFn assembliesGet =
                        g_il2cppHandle
                            ? (AssembliesFn)dlsym(g_il2cppHandle, "il2cpp_domain_get_assemblies")
                            : nullptr;
                    if (domainGet && assembliesGet) {
                        void *domain = domainGet();
                        size_t count = 0;
                        const void **assemblies = assembliesGet(domain, &count);
                        Emit("il2cpp assemblies: domain=%p count=%zu first=%p", domain, count,
                             (count && assemblies) ? assemblies[0] : nullptr);

                        using ImageFn = void *(*)(const void *);
                        using NameFn = const char *(*)(const void *);
                        using ClassCountFn = size_t (*)(const void *);
                        ImageFn assemblyImage =
                            (ImageFn)dlsym(g_il2cppHandle, "il2cpp_assembly_get_image");
                        NameFn imageName = (NameFn)dlsym(g_il2cppHandle, "il2cpp_image_get_name");
                        ClassCountFn classCount =
                            (ClassCountFn)dlsym(g_il2cppHandle, "il2cpp_image_get_class_count");
                        Emit("il2cpp image api: image=%p name=%p classCount=%p", (void *)assemblyImage,
                             (void *)imageName, (void *)classCount);

                        if (assemblyImage && imageName) {
                            for (size_t index = 0; index < count && index < 96; index++) {
                                void *image = assemblyImage(assemblies[index]);
                                const char *name = image ? imageName(image) : "(null)";
                                size_t classes =
                                    (classCount && image) ? classCount(image) : (size_t)-1;
                                if (index < 8 || (name && strstr(name, "Assembly-CSharp"))) {
                                    Emit("il2cpp image #%zu %s classes=%zu", index,
                                         name ? name : "(null)", classes);
                                }

                                /* Methods of the boot controller: the likely managed entry point. */
                                if (name && strcmp(name, "Assembly-CSharp.dll") == 0) {
                                    using FromNameFn = void *(*)(const void *, const char *, const char *);
                                    using MethodsFn = const void *(*)(void *, void **);
                                    using MethodNameFn = const char *(*)(const void *);
                                    using ParamCountFn = uint16_t (*)(const void *);
                                    FromNameFn classFromName =
                                        (FromNameFn)dlsym(g_il2cppHandle, "il2cpp_class_from_name");
                                    MethodsFn classMethods =
                                        (MethodsFn)dlsym(g_il2cppHandle, "il2cpp_class_get_methods");
                                    MethodNameFn methodName =
                                        (MethodNameFn)dlsym(g_il2cppHandle, "il2cpp_method_get_name");
                                    ParamCountFn paramCount = (ParamCountFn)dlsym(
                                        g_il2cppHandle, "il2cpp_method_get_param_count");
                                    if (classFromName && classMethods && methodName) {
                                        const char *wanted[] = { "BootSceneControl", "UIInitializer" };
                                        for (const char *type : wanted) {
                                            void *klass = classFromName(image, "", type);
                                            Emit("il2cpp lookup %s -> %p", type, klass);
                                            if (!klass) continue;
                                            void *iterator = nullptr;
                                            int shown = 0;
                                            while (shown < 16) {
                                                const void *method = classMethods(klass, &iterator);
                                                if (!method) break;
                                                Emit("il2cpp method %s::%s params=%d", type,
                                                     methodName(method),
                                                     paramCount ? (int)paramCount(method) : -1);
                                                shown++;
                                            }
                                        }
                                    } else {
                                        Emit("il2cpp method api missing: fromName=%p methods=%p name=%p",
                                             (void *)classFromName, (void *)classMethods,
                                             (void *)methodName);
                                    }
                                }

                                /* For the game assembly, list type names to look for an entry point. */
                                if (name && strcmp(name, "Assembly-CSharp.dll") == 0 && classCount) {
                                    using ImageClassFn = void *(*)(const void *, size_t);
                                    using ClassNameFn = const char *(*)(void *);
                                    using ClassNsFn = const char *(*)(void *);
                                    ImageClassFn imageClass =
                                        (ImageClassFn)dlsym(g_il2cppHandle, "il2cpp_image_get_class");
                                    ClassNameFn className =
                                        (ClassNameFn)dlsym(g_il2cppHandle, "il2cpp_class_get_name");
                                    ClassNsFn classNamespace =
                                        (ClassNsFn)dlsym(g_il2cppHandle, "il2cpp_class_get_namespace");
                                    Emit("il2cpp class api: get=%p name=%p ns=%p",
                                         (void *)imageClass, (void *)className,
                                         (void *)classNamespace);
                                    if (imageClass && className) {
                                        for (size_t cls = 0; cls < classes; cls++) {
                                            void *klass = imageClass(image, cls);
                                            if (!klass) continue;
                                            const char *typeName = className(klass);
                                            if (!typeName) continue;
                                            /* Only report types whose names suggest a start-up entry. */
                                            if (strstr(typeName, "Main") || strstr(typeName, "Game") ||
                                                strstr(typeName, "Boot") || strstr(typeName, "Launch") ||
                                                strstr(typeName, "Entry") || strstr(typeName, "Init")) {
                                                const char *ns =
                                                    classNamespace ? classNamespace(klass) : "";
                                                Emit("il2cpp class #%zu %s%s%s", cls,
                                                     ns ? ns : "",
                                                     (ns && ns[0]) ? "." : "", typeName);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        Emit("il2cpp assembly query unavailable (domain=%p get=%p)", (void *)domainGet,
                             (void *)assembliesGet);
                    }
                } else {
                    Emit("explicit il2cpp_init unavailable");
                }
            } else {
                /* Let the engine initialise the scripting runtime itself during start-up. */
                Emit("manual il2cpp_init skipped (engine will initialise it)");
            }
        }
    }

    void *initJni = nullptr;
    for (const auto &entry : g_registered) {
        if (entry.name == "initJni") {
            initJni = entry.fnPtr;
            break;
        }
    }
    if (!initJni) {
        Emit("initJni not registered; stopping");
        return g_log.c_str();
    }

    FakeObject *context = NewObject("android/content/Context");
    g_currentActivity=context;
    /* Receiver for the instance natives registered on com/unity3d/player/UnityPlayer. */
    FakeObject *player = NewObject("com/unity3d/player/UnityPlayer");
    player->refs["mContext"] = context;
    using InitJniFn = void (*)(void *, void *, void *);
    InitJniFn init = (InitJniFn)initJni;
    Emit("calling UnityPlayer.initJni(context) ...");
    g_traceJni = true;
    g_traceJniCount = 0;
    init(&g_env, player, context);
    g_traceJni = false;
    Emit("UnityPlayer.initJni returned (%d framework calls traced)", g_traceJniCount);

    Emit("APK_ARCHIVE readable=%d path=/data/storage/el2/base/files/rea-original-phigros.apk", access("/data/storage/el2/base/files/rea-original-phigros.apk", R_OK) == 0);
    /* The engine only starts when Java tells it a surface exists and then renders frames. */
    void *recreateGfx = nullptr;
    void *render = nullptr;
    void *surfaceChanged = nullptr;
    for (const auto &entry : g_registered) {
        if (entry.name == "nativeRecreateGfxState") recreateGfx = entry.fnPtr;
        if (entry.name == "nativeRender") render = entry.fnPtr;
        if (entry.name == "nativeSendSurfaceChangedEvent") surfaceChanged = entry.fnPtr;
    }

    extern void *ReaShim_GetNativeWindow(void);
    void *window = ReaShim_GetNativeWindow();
    Emit("OHNativeWindow published: %p", window);

    if (recreateGfx && !window) {
        Emit("skipping nativeRecreateGfxState: no OHNativeWindow yet");
        recreateGfx = nullptr;
        render = nullptr;
        surfaceChanged = nullptr;
    }

    // initJni has completed in the UI callback. The original Java player then
    // starts UnityMain; retain that separation and keep all graphics/resume/
    // frame jobs on one host-owned thread instead of blocking ArkUI.
    if (recreateGfx && render && window) {
        auto *start=new UnityHostStart{player,NewObject("android/view/Surface"),recreateGfx,render,surfaceChanged,nullptr,nullptr,nullptr,nullptr};
        for (const auto &entry:g_registered) {
            if (entry.name=="nativeFocusChanged")start->focus=entry.fnPtr;
            if (entry.name=="nativeResume")start->resume=entry.fnPtr;
            if (entry.name=="nativeInjectEvent")start->inject=entry.fnPtr;
            if (entry.name=="nativePause")start->pause=entry.fnPtr;
        }
        pthread_t host;
        int result=pthread_create(&host,nullptr,UnityHostBody,start);
        if (result==0) {pthread_detach(host);return "Original engine host scheduled; see UNITY_HOST logs.";}
        delete start;Emit("UNITY_HOST create failed=%d",result);
        return "Original engine host could not start.";
    }

    if (recreateGfx) {
        StartFaultWatchdog();
        {
            unsigned long unityBase2 = 0;
            for (const auto &entry : g_registered) {
                if (entry.name == "initJni") {
                    unityBase2 = (unsigned long)(uintptr_t)entry.fnPtr - 0x7567d4UL;
                    break;
                }
            }
            if (unityBase2) {
                /* The only `br` in the reachable subtree: a vtable tail call off a global object. */
                {
                    unsigned long globalAddr = unityBase2 + 0x1408558UL;
                    unsigned long object;
                    memcpy(&object, (void *)globalAddr, sizeof(object));
                    Emit("BRCHECK global[0x1408558]=0x%lx", object);
                    if (object > 0x1000UL && object < 0x8000000000UL) {
                        unsigned long vtable;
                        memcpy(&vtable, (void *)object, sizeof(vtable));
                        Emit("BRCHECK object=0x%lx vtable=0x%lx", object, vtable);
                        if (vtable > 0x1000UL && vtable < 0x8000000000UL) {
                            unsigned long slot;
                            memcpy(&slot, (void *)(vtable + 0x28), sizeof(slot));
                            Dl_info slotInfo;
                            memset(&slotInfo, 0, sizeof(slotInfo));
                            if (slot && dladdr((void *)slot, &slotInfo) && slotInfo.dli_fname) {
                                Emit("BRCHECK vtable[0x28]=0x%lx -> %s+0x%lx", slot, slotInfo.dli_fname,
                                     slot - (unsigned long)(uintptr_t)slotInfo.dli_fbase);
                            } else {
                                Emit("BRCHECK vtable[0x28]=0x%lx (unattributed)", slot);
                            }
                        }
                    }
                }
                unsigned long offsets[4] = { 0x133e2a0, 0x133e2a8, 0x133e3a0, 0x133e3b0 };
                for (unsigned long offset : offsets) {
                    unsigned long value;
                    memcpy(&value, (void *)(unityBase2 + offset), sizeof(value));
                    Emit("PREGFX got[0x%lx]=0x%lx", offset, value);
                }
            }
        }
        FakeObject *surface = NewObject("android/view/Surface");
        using GfxFn = void (*)(void *, void *, int, void *);

        /*
         * Unity's Java layer performs these between surface creation and the first frame. Without them the
         * platform objects that nativeRender dereferences are never constructed.
         */
        {
            void *focusChanged = nullptr;
            void *resumeFn = nullptr;
            void *surfaceChanged = nullptr;
            for (const auto &entry : g_registered) {
                if (entry.name == "nativeFocusChanged") focusChanged = entry.fnPtr;
                else if (entry.name == "nativeResume") resumeFn = entry.fnPtr;
                else if (entry.name == "nativeSendSurfaceChangedEvent") surfaceChanged = entry.fnPtr;
            }
            if (focusChanged) {
                ((void (*)(void *, void *, unsigned char))focusChanged)(&g_env, player, 1);
                Emit("nativeFocusChanged(1) returned");
            }
            if (resumeFn) {
                ((void (*)(void *, void *))resumeFn)(&g_env, player);
                Emit("nativeResume() returned");
                SampleRegistry("after nativeResume");
            }
            if (surfaceChanged) {
                ((void (*)(void *, void *))surfaceChanged)(&g_env, player);
                Emit("nativeSendSurfaceChangedEvent() returned");
            }
        }

        g_traceJni = true;
        g_traceJniCount = 0;
        InstallRegistryPlaceholder();
        if (g_traceGfx && g_gfxOnWorker) {
            g_renderForWorker = render;
            static TracedCall workerCall;
            workerCall.target = recreateGfx;
            workerCall.env = &g_env;
            workerCall.receiver = player;
            workerCall.arg = 0;
            workerCall.objectArg = surface;
            pthread_t worker;
            Emit("calling nativeRecreateGfxState(0, surface) on a worker thread ...");
            if (pthread_create(&worker, nullptr, GfxWorker, &workerCall) == 0) {
                pthread_detach(worker);
                return g_log.c_str();
            }
            Emit("worker thread could not be created; calling inline");
        }

        if (g_traceGfx) {
            Emit("calling nativeRecreateGfxState(0, surface) under ptrace ...");
            TracedCall call;
            call.target = recreateGfx;
            call.env = &g_env;
            call.receiver = player;
            call.arg = 0;
            call.objectArg = surface;
            bool faulted = RunTraced(&call);
            g_traceJni = false;
            Emit("traced graphics call finished, faulted=%d (%d framework calls)", faulted ? 1 : 0,
                 g_traceJniCount);
            if (g_unityBase) {
                /* The render path calls a global singleton through its vtable. Report what these
                 * slots actually hold, so a null vtable is identified rather than inferred. */
                unsigned long slotA = *(unsigned long *)(uintptr_t)(g_unityBase + 0x133cbc8UL);
                unsigned long slotB = *(unsigned long *)(uintptr_t)(g_unityBase + 0x133cbd0UL);
                unsigned long slotC = *(unsigned long *)(uintptr_t)(g_unityBase + 0x133cbd8UL);
                Emit("SINGLETON slotA=0x%lx slotB=0x%lx slotC=0x%lx", slotA, slotB, slotC);
                if (slotA) Emit("SINGLETON slotA[0]=0x%lx", *(unsigned long *)(uintptr_t)slotA);
                if (slotB) Emit("SINGLETON slotB[0]=0x%lx", *(unsigned long *)(uintptr_t)slotB);
                if (slotC) Emit("SINGLETON slotC[0]=0x%lx", *(unsigned long *)(uintptr_t)slotC);

                /*
                 * Unity resolves these through its own module table and the lookups came back null, yet
                 * the render path calls them unguarded. Supply the real addresses from libil2cpp.
                 */
                ReaPrepareIl2cppData();
                int repaired = FillIl2CppSlots(g_unityBase);
                Emit("IL2CPP_SLOTS repaired before rendering: %d", repaired);
                if (slotA && *(unsigned long *)(uintptr_t)slotA == 0) {
                    void *symbol = g_il2cppHandle ? dlsym(g_il2cppHandle, "il2cpp_register_log_callback")
                                                  : nullptr;
                    if (symbol) {
                        *(unsigned long *)(uintptr_t)slotA = (unsigned long)(uintptr_t)symbol;
                        Emit("SLOT_FIX slotA -> il2cpp_register_log_callback = %p", symbol);
                    } else {
                        Emit("SLOT_FIX slotA failed: symbol unavailable");
                    }
                }
                if (slotB && *(unsigned long *)(uintptr_t)slotB == 0) {
                    void *symbol = g_il2cppHandle
                                       ? dlsym(g_il2cppHandle, "il2cpp_unity_set_android_network_up_state_func")
                                       : nullptr;
                    if (symbol) {
                        *(unsigned long *)(uintptr_t)slotB = (unsigned long)(uintptr_t)symbol;
                        Emit("SLOT_FIX slotB -> il2cpp_unity_set_android_network_up_state_func = %p", symbol);
                    } else {
                        Emit("SLOT_FIX slotB failed: symbol unavailable");
                    }
                }
            }
            if (!faulted && render) {
                /*
                 * Run the first frame under ptrace: the in-process handler's program counter is not
                 * trustworthy for this crash, while ptrace reports the kernel's own registers.
                 */
                TracedCall renderCall{};
                renderCall.target = render;
                renderCall.env = &g_env;
                renderCall.receiver = player;
                renderCall.arg = 0;
                renderCall.objectArg = nullptr;
                if (g_unityBase) {
                    /*
                     * The engine never registers its subsystems, yet the first render path dereferences
                     * registry[0] at +0xc0. A zeroed placeholder keeps execution moving so the next real
                     * failure becomes visible.
                     */

                    /* Registry of subsystem singletons indexed by an integer. */
                    {
                        unsigned long *table = (unsigned long *)(uintptr_t)(g_unityBase + 0x1379148UL);
                        Emit("REGISTRY %lx %lx %lx %lx %lx %lx %lx %lx", table[0], table[1], table[2],
                             table[3], table[4], table[5], table[6], table[7]);
                    }

                    /* Platform interface object used by nativeRender's callback path. */
                    {
                        unsigned long *cell = (unsigned long *)(uintptr_t)(g_unityBase + 0x133bbc8UL);
                        unsigned long object = *cell;
                        Emit("IFACE slot=0x%lx object=0x%lx", (unsigned long)(uintptr_t)cell, object);
                        if (object >= g_unityBase && object < g_unityBase + 0x8000000UL) {
                            unsigned long *words = (unsigned long *)(uintptr_t)object;
                            Emit("IFACE words %lx %lx %lx %lx", words[0], words[1], words[2], words[3]);
                        }
                    }

                    /* Experiment: call the registration root directly and see if the registry fills. */
                    if (g_callChainRoot) {
                        Emit("CALLROOT invoking libunity+0x5d7c18(null)");
                        using RootFn = void (*)(void *);
                        ((RootFn)(uintptr_t)(g_unityBase + 0x5d7c18UL))(nullptr);
                        Emit("CALLROOT returned");
                        unsigned long *after = (unsigned long *)(uintptr_t)(g_unityBase + 0x1379148UL);
                        Emit("CALLROOT registry after: %lx %lx %lx %lx", after[0], after[1], after[2],
                             after[3]);
                    }

                    /* Dispatcher 0x6AB5B0 walks a handler array; a null entry jumps to address zero. */
                    {
                        unsigned long *listSlot = (unsigned long *)(uintptr_t)(g_unityBase + 0x13c7ed8UL);
                        unsigned long *countSlot = (unsigned long *)(uintptr_t)(g_unityBase + 0x13c7ee8UL);
                        unsigned long listBase = *listSlot;
                        unsigned long count = *countSlot;
                        Emit("HANDLERS base=0x%lx count=%lu", listBase, count);
                        if (listBase >= g_unityBase && listBase < g_unityBase + 0x8000000UL) {
                            unsigned long *entries = (unsigned long *)(uintptr_t)listBase;
                            unsigned long shown = count < 8 ? count : 8;
                            for (unsigned long i = 0; i < shown; i++) {
                                Emit("HANDLERS [%lu] = 0x%lx", i, entries[i]);
                            }
                        }
                    }

                    /* Memory callbacks live behind slot 0x133c918; dump a few words of that struct. */
                    unsigned long *cell7f8 = (unsigned long *)(uintptr_t)(g_unityBase + 0x133c918UL);
                    unsigned long object7f8 = *cell7f8;
                    Emit("MEMCB slot=0x%lx object=0x%lx", (unsigned long)(uintptr_t)cell7f8, object7f8);
                    if (object7f8 >= g_unityBase && object7f8 < g_unityBase + 0x8000000UL) {
                        unsigned long *words = (unsigned long *)(uintptr_t)object7f8;
                        Emit("MEMCB words %lx %lx %lx %lx %lx %lx", words[0], words[1], words[2], words[3],
                             words[4], words[5]);
                    }
                }
                if (g_directRender) {
                    using DirectRenderFn = unsigned char (*)(void *, void *);
                    Emit("calling nativeRender directly (in-process)");
                    for (int frame = 0; frame < 30; frame++) {
                        unsigned char drawn = ((DirectRenderFn)render)(&g_env, player);
                        Emit("nativeRender #%d -> %u", frame, (unsigned)drawn);
                        if (!drawn) break;
                    }
                    Emit("direct render loop finished");
                    return g_log.c_str();
                }

                Emit("tracing the first nativeRender call");
                bool renderFaulted = RunTraced(&renderCall);
                Emit("traced nativeRender finished, faulted=%d", renderFaulted ? 1 : 0);

                if (!renderFaulted) {
                    /* The device came up, so drive frames as the Java layer would. */
                    using RenderFn = unsigned char (*)(void *, void *);
                    for (int frame = 0; frame < 40; frame++) {
                        unsigned char drawn = ((RenderFn)render)(&g_env, player);
                        Emit("nativeRender #%d -> %u", frame, (unsigned)drawn);
                        if (!drawn) break;
                    }
                }
            }
        } else {
            Emit("calling nativeRecreateGfxState(0, surface) ...");
            ((GfxFn)recreateGfx)(&g_env, player, 0, surface);
            Emit("nativeRecreateGfxState returned");
            ReaLogEglState("after nativeRecreateGfxState");
            if (g_skipFrameSync) SkipFrameSync();
            if (g_callSetWindow) CallSetWindow();
            if (g_fillAsyncSlot) FillAsyncResultSlot();
            Emit("threads after graphics init = %d", CountThreads());
            if (render) {
                using RenderFn = unsigned char (*)(void *, void *);
                if (g_renderOnUnityThread) {
                    int unityTid = -1;
                    for (int attempt = 0; attempt < 30 && unityTid < 0; attempt++) {
                        unityTid = FindThreadByName("UnityMain");
                        if (unityTid < 0) usleep(100000);
                    }
                    Emit("UnityMain tid = %d", unityTid);
                    if (unityTid > 0) {
                        struct sigaction action;
                        memset(&action, 0, sizeof(action));
                        action.sa_handler = RenderOnUnityThreadHandler;
                        action.sa_flags = SA_RESTART;
                        sigemptyset(&action.sa_mask);
                        sigaction(SIGUSR2, &action, nullptr);
                        g_renderTarget = (void *)render;
                        g_renderEnv = &g_env;
                        g_renderPlayer = player;
                        for (int frame = 0; frame < 10; frame++) {
                            g_renderFinished = 0;
                            g_renderRequested = 1;
                            syscall(SYS_tgkill, getpid(), unityTid, SIGUSR2);
                            int waited = 0;
                            while (!g_renderFinished && waited < 100) {
                                usleep(50000);
                                waited++;
                            }
                            if (!g_renderFinished) {
                                Emit("render on UnityMain #%d did not complete", frame);
                                break;
                            }
                            Emit("render on UnityMain #%d completed", frame);
                        }
                        Emit("render on UnityMain finished");
                        return g_log.c_str();
                    }
                }
                if (surfaceChanged) {
                    ((void (*)(void *, void *))surfaceChanged)(&g_env, player);
                    Emit("nativeSendSurfaceChangedEvent() repeated before the first frame");
                }
                Emit("starting render frames on the main thread");
                /* The rescue experiment showed a predicate wait, so it stays disabled. */
                g_traceJni = true;
                g_traceJniCount = 0;
                g_frameThread = pthread_self();
                g_frameThreadValid = true;
                StartHangWatchdog();
                for (int frame = 0; frame < 30; frame++) {
                    g_frameInProgress = true;
                    g_hangReported = false;
                    unsigned char drawn = ((RenderFn)render)(&g_env, player);
                    g_frameInProgress = false;
                    Emit("nativeRender #%d -> %u", frame, (unsigned)drawn);
                    if (!drawn) break;
                }
                Emit("main-thread render loop finished");
            }
        }
    }

    if (surfaceChanged) {
        using VoidFn = void (*)(void *, void *);
        Emit("calling nativeSendSurfaceChangedEvent() ...");
        ((VoidFn)surfaceChanged)(&g_env, player);
        Emit("nativeSendSurfaceChangedEvent returned");
    }

    if (render) {
        using RenderFn = unsigned char (*)(void *, void *);
        for (int frame = 0; frame < 8; frame++) {
            unsigned char drawn = ((RenderFn)render)(&g_env, player);
            Emit("nativeRender #%d -> %u", frame, (unsigned)drawn);
        }
    }

    Emit("registered natives: %zu", g_registered.size());
    for (const auto &entry : g_registered) {
        Emit("  REG %s :: %s %s", entry.className.c_str(), entry.name.c_str(), entry.signature.c_str());
    }
    Emit("=== unity bootstrap probe end ===");
    return g_log.c_str();
}
/* Runs the bootstrap with the graphics call executed in a traced child. */
extern "C" const char *ReaJniArgsSelfTest();
extern "C" const char *ReaRunTracedGfx()
{
    InitializeTypedJni();
    if(!rea_prefs::SelfTest()){Emit("PREFS_DISK_SELFTEST FAIL engine startup stopped");return g_log.c_str();}
    const char *argumentsReport=ReaJniArgsSelfTest();
    Emit("%s", argumentsReport);
    if (!strstr(argumentsReport, "JNI_ARGS_SELFTEST PASS")) return g_log.c_str();
    if (!VerifyJniIdentityContract()) return g_log.c_str();
    if (!VerifyFrameworkDefaultContract()) return g_log.c_str();
    if (!VerifyConstructionContract()) return g_log.c_str();
    if (!VerifyFloatingContract()) return g_log.c_str();
    if (!VerifyReflectionContract()) return g_log.c_str();
    if (!VerifyPreferenceContract()) return g_log.c_str();
    g_traceGfx = false;
    ReaUnityProbe();
    static thread_local std::string snapshot;
    {std::lock_guard<std::mutex> lock(g_logMutex);snapshot=g_log;}
    return snapshot.c_str();
}

/* ---- breakpoint + single-step tracer ------------------------------------------------------ */

/*
 * Sets a BRK at an absolute address in the traced child, then single-steps from the hit while logging
 * pc/x30/sp. This answers the one remaining question: which instruction leaves x30 at zero before the
 * `ret` that jumps to address 0.
 */
namespace {

struct StepLog {
    unsigned long pc;
    unsigned long x30;
    unsigned long sp;
};

bool ReadMemory(pid_t child, unsigned long address, unsigned long *value)
{
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, child, (void *)address, 0);
    if (word == -1 && errno != 0) return false;
    *value = (unsigned long)word;
    return true;
}

bool WriteMemory(pid_t child, unsigned long address, unsigned long value)
{
    return ptrace(PTRACE_POKEDATA, child, (void *)address, (void *)value) == 0;
}

bool GetRegs(pid_t child, struct user_pt_regs *regs)
{
    struct iovec io;
    io.iov_base = regs;
    io.iov_len = sizeof(*regs);
    return ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &io) == 0;
}

bool SetPcAndContinue(pid_t child, unsigned long pc)
{
    struct user_pt_regs regs;
    if (!GetRegs(child, &regs)) return false;
    regs.pc = pc;
    struct iovec io;
    io.iov_base = &regs;
    io.iov_len = sizeof(regs);
    if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &io) != 0) return false;
    return true;
}

} // namespace

/*
 * Traced run with a breakpoint at libunity+breakpointOffset. Returns after the child faults or exits.
 */
extern "C" const char *ReaBreakpointTrace()
{
    g_log.clear();
    g_vm.functions = g_vmFunctions;
    Emit("=== breakpoint trace ===");

    void *main = dlopen("libmain.so", RTLD_NOW | RTLD_GLOBAL);
    if (!main) {
        Emit("LOAD_FAIL libmain.so");
        return g_log.c_str();
    }
    using JniOnLoadFn = int (*)(void *, void *);
    JniOnLoadFn onLoad = (JniOnLoadFn)dlsym(main, "JNI_OnLoad");
    if (!onLoad) {
        Emit("SYM_MISSING libmain JNI_OnLoad");
        return g_log.c_str();
    }
    onLoad(&g_vm, nullptr);

    void *loadFn = nullptr;
    for (const auto &entry : g_registered) {
        if (entry.name == "load") { loadFn = entry.fnPtr; break; }
    }
    if (!loadFn) {
        Emit("NativeLoader.load not registered");
        return g_log.c_str();
    }

    const char *engineDir = "/data/storage/el1/bundle/libs/arm64";
    FakeObject *pathString = NewObject("java/lang/String");
    pathString->strs["value"] = engineDir;
    FakeObject *loaderClass = NewObject("java/lang/Class");
    loaderClass->strs["name"] = "com/unity3d/player/NativeLoader";
    using LoadFn = unsigned char (*)(void *, void *, void *);
    ((LoadFn)loadFn)(&g_env, loaderClass, pathString);

    void *recreateGfx = nullptr;
    for (const auto &entry : g_registered) {
        if (entry.name == "nativeRecreateGfxState") { recreateGfx = entry.fnPtr; break; }
    }
    if (!recreateGfx) {
        Emit("nativeRecreateGfxState not registered");
        return g_log.c_str();
    }

    /* Derive libunity's base from a native that provably lives in it. */
    unsigned long unityBase = 0;
    for (const auto &entry : g_registered) {
        if (entry.name == "initJni") {
            unityBase = (unsigned long)(uintptr_t)entry.fnPtr - 0x7567d4UL;
            g_unityBase = unityBase;
            break;
        }
    }
    g_unityBase = unityBase;
    if (!unityBase) {
        Emit("cannot derive libunity base");
        return g_log.c_str();
    }

    const unsigned long breakpoints[3] = { 0x73cb14UL, 0x7402ecUL, 0x756a08UL };
    const char *breakpointNames[3] = { "0x73cb14 device-create", "0x7402ec guard1",
                                       "0x756a08 recreateGfx entry" };

    for (int which = 0; which < 3; which++) {
        unsigned long target = unityBase + breakpoints[which];
        Emit("--- trying breakpoint at %s (0x%lx)", breakpointNames[which], target);

        pid_t child = fork();
        if (child == 0) {
            signal(SIGSEGV, SIG_DFL);
            signal(SIGBUS, SIG_DFL);
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) _exit(2);
            raise(SIGSTOP);
            using GfxFn = void (*)(void *, void *, int, void *);
            FakeObject *surface = NewObject("android/view/Surface");
            FakeObject *player = NewObject("com/unity3d/player/UnityPlayer");
            ((GfxFn)recreateGfx)(&g_env, player, 0, surface);
            _exit(0);
        }
        if (child < 0) {
            Emit("fork failed");
            return g_log.c_str();
        }

        int status = 0;
        waitpid(child, &status, 0);
        if (!WIFSTOPPED(status)) { Emit("child did not stop"); return g_log.c_str(); }

        unsigned long original = 0;
        bool planted = ReadMemory(child, target, &original) && WriteMemory(child, target, 0xD4200000UL);

        ptrace(PTRACE_CONT, child, 0, 0);

        bool hit = false;
        struct StepLog steps[64];
        int stepCount = 0;

        for (;;) {
            if (waitpid(child, &status, 0) < 0) break;
            if (WIFEXITED(status)) break;
            if (!WIFSTOPPED(status)) break;

            struct user_pt_regs regs;
            if (!GetRegs(child, &regs)) break;

            if (!hit) {
                /* The kernel reports either the BRK address or the one after it, depending on
                 * how the trap is delivered; accept both rather than miss the hit. */
                if (regs.pc == target || regs.pc == target + 4) {
                    hit = true;
                    if (planted) WriteMemory(child, target, original);
                    SetPcAndContinue(child, target);
                    ptrace(PTRACE_SINGLESTEP, child, 0, 0);
                    continue;
                }
                if (WSTOPSIG(status) == SIGTRAP) {
                    /* A trap we did not plant: step over it instead of killing the child. */
                    ptrace(PTRACE_CONT, child, 0, 0);
                    continue;
                }
                /* Any other signal is the real failure: report where it happened. */
                Dl_info info;
                memset(&info, 0, sizeof(info));
                const char *module = "?";
                unsigned long offset = 0;
                if (regs.pc && dladdr((void *)regs.pc, &info) && info.dli_fname) {
                    module = info.dli_fname;
                    offset = (unsigned long)regs.pc - (unsigned long)(uintptr_t)info.dli_fbase;
                }
                Emit("child faulted before the breakpoint: signal=%d pc=0x%llx (%s+0x%lx)",
                     WSTOPSIG(status), (unsigned long long)regs.pc, module, offset);
                break;
            }

            if (stepCount < 64) {
                steps[stepCount].pc = (unsigned long)regs.pc;
                steps[stepCount].x30 = (unsigned long)regs.regs[30];
                steps[stepCount].sp = (unsigned long)regs.sp;
                stepCount++;
            }
            if (regs.pc == 0) {
                Emit("reached pc=0 after %d steps", stepCount);
                break;
            }
            if (stepCount >= 64) {
                Emit("step budget exhausted inside the function");
                break;
            }
            ptrace(PTRACE_SINGLESTEP, child, 0, 0);
        }

        for (int i = 0; i < stepCount; i++) {
            Dl_info info;
            memset(&info, 0, sizeof(info));
            const char *module = "?";
            unsigned long offset = 0;
            if (steps[i].pc && dladdr((void *)steps[i].pc, &info) && info.dli_fname) {
                module = info.dli_fname;
                offset = steps[i].pc - (unsigned long)(uintptr_t)info.dli_fbase;
            }
            Emit("STEP %02d pc=0x%lx (%s+0x%lx) x30=0x%lx sp=0x%lx", i, steps[i].pc, module, offset,
                 steps[i].x30, steps[i].sp);
        }

        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        if (hit) {
            Emit("breakpoint %s produced a trace; stopping here", breakpointNames[which]);
            break;
        }
        Emit("breakpoint %s was not hit; child ended first", breakpointNames[which]);
    }

    Emit("=== breakpoint trace end ===");
    return g_log.c_str();
}
