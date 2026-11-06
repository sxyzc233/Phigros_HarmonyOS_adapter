#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <hilog/log.h>

#define TRACE(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, 0, "TransEGL", "tid=%{public}ld " fmt, (long)syscall(SYS_gettid), ##__VA_ARGS__)
namespace {
enum Slot { GetDisplay, Initialize, ChooseConfig, BindAPI, CreateContext, CreateWindowSurface,
    CreatePbufferSurface, MakeCurrent, GetError, GetProcAddress, GetString, SwapBuffers, Count };
std::atomic<void*> originals[Count]{};
template<class T> T real(Slot s) { return reinterpret_cast<T>(originals[s].load(std::memory_order_acquire)); }
}
extern "C" void *ReaObserveGraphicsSymbol(const char *, void *);
extern "C" void ReaLogEglState(const char *stage)
{
    // Read TLS state only. Do not consume EGL/GL errors during engine execution.
    TRACE("EGL_STATE %{public}s display=%{public}p context=%{public}p draw=%{public}p read=%{public}p",
          stage, eglGetCurrentDisplay(), eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW), eglGetCurrentSurface(EGL_READ));
}
namespace {
EGLDisplay getDisplay(EGLNativeDisplayType native)
{
    auto result=real<decltype(&eglGetDisplay)>(GetDisplay)(native);
    TRACE("eglGetDisplay native=%{public}p result=%{public}p",(void*)native,result);return result;
}
EGLBoolean initialize(EGLDisplay d,EGLint *major,EGLint *minor)
{
    auto result=real<decltype(&eglInitialize)>(Initialize)(d,major,minor);
    TRACE("eglInitialize display=%{public}p result=%{public}u version=%{public}d.%{public}d",d,result, result&&major?*major:-1,result&&minor?*minor:-1);return result;
}
EGLBoolean chooseConfig(EGLDisplay d,const EGLint *attrs,EGLConfig *configs,EGLint size,EGLint *count)
{
    auto result=real<decltype(&eglChooseConfig)>(ChooseConfig)(d,attrs,configs,size,count);
    TRACE("eglChooseConfig display=%{public}p result=%{public}u size=%{public}d count=%{public}d first=%{public}p",d,result,size,result&&count?*count:-1,result&&count&&*count&&configs&&size>0?configs[0]:nullptr);return result;
}
EGLBoolean bindAPI(EGLenum api)
{
    auto result=real<decltype(&eglBindAPI)>(BindAPI)(api);
    TRACE("eglBindAPI api=%{public}x result=%{public}u",api,result);return result;
}
EGLContext createContext(EGLDisplay d,EGLConfig c,EGLContext share,const EGLint *attrs)
{
    auto result=real<decltype(&eglCreateContext)>(CreateContext)(d,c,share,attrs);
    TRACE("eglCreateContext display=%{public}p config=%{public}p share=%{public}p result=%{public}p",d,c,share,result);return result;
}
EGLSurface createWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType w,const EGLint *attrs)
{
    auto result=real<decltype(&eglCreateWindowSurface)>(CreateWindowSurface)(d,c,w,attrs);
    TRACE("eglCreateWindowSurface display=%{public}p config=%{public}p window=%{public}p result=%{public}p",d,c,(void*)w,result);return result;
}
EGLSurface createPbufferSurface(EGLDisplay d,EGLConfig c,const EGLint *attrs)
{
    auto result=real<decltype(&eglCreatePbufferSurface)>(CreatePbufferSurface)(d,c,attrs);
    TRACE("eglCreatePbufferSurface display=%{public}p config=%{public}p result=%{public}p",d,c,result);return result;
}
EGLBoolean makeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext context)
{
    auto result=real<decltype(&eglMakeCurrent)>(MakeCurrent)(d,draw,read,context);
    TRACE("eglMakeCurrent display=%{public}p draw=%{public}p read=%{public}p context=%{public}p result=%{public}u",d,draw,read,context,result);
    ReaLogEglState("after engine eglMakeCurrent");return result;
}
EGLint getError()
{
    EGLint result=real<decltype(&eglGetError)>(GetError)();TRACE("engine eglGetError=%{public}x",result);return result;
}
const GLubyte *getString(GLenum kind)
{
    auto result=real<const GLubyte *(*)(GLenum)>(GetString)(kind);
    if(kind==GL_VENDOR||kind==GL_RENDERER||kind==GL_VERSION) {
        TRACE("glGetString kind=%{public}x ptr=%{public}p value=%{public}s",kind,result,result?reinterpret_cast<const char*>(result):"(null)");
        ReaLogEglState("glGetString");
        if(kind==GL_RENDERER && !result) {
            static bool examined=false;
            if(!examined) {
                examined=true;
                TRACE("GL_API_COMPARE bound_api=%{public}x",eglQueryAPI());
                using GetStringFn=const GLubyte *(*)(GLenum);
                auto proc=reinterpret_cast<GetStringFn>(eglGetProcAddress("glGetString"));
                Dl_info pinfo{};if(proc)dladdr(reinterpret_cast<void*>(proc),&pinfo);
                const GLubyte *pv=proc&&proc!=getString?proc(kind):nullptr;
                TRACE("GL_API_COMPARE eglGetProcAddress fn=%{public}p library=%{public}s value=%{public}s",reinterpret_cast<void*>(proc),pinfo.dli_fname?pinfo.dli_fname:"?",pv?reinterpret_cast<const char*>(pv):"(null)");
                void *gl3=dlopen("libGLESv3.so",RTLD_NOW|RTLD_LOCAL);
                const char *loadError=gl3?nullptr:dlerror();
                TRACE("GL_API_COMPARE GLESv3 handle=%{public}p error=%{public}s",gl3,loadError?loadError:"");
                auto fn3=gl3?reinterpret_cast<GetStringFn>(dlsym(gl3,"glGetString")):nullptr;
                Dl_info info3{};if(fn3)dladdr(reinterpret_cast<void*>(fn3),&info3);
                const GLubyte *v3=fn3&&fn3!=getString?fn3(kind):nullptr;
                TRACE("GL_API_COMPARE GLESv3 fn=%{public}p library=%{public}s value=%{public}s",reinterpret_cast<void*>(fn3),info3.dli_fname?info3.dli_fname:"?",v3?reinterpret_cast<const char*>(v3):"(null)");
                auto again=real<GetStringFn>(GetString)(kind);
                TRACE("GL_API_COMPARE GLESv2 after library load value=%{public}s",again?reinterpret_cast<const char*>(again):"(null)");
            }
        }
    }
    return result;
}
__eglMustCastToProperFunctionPointerType getProcAddress(const char *name)
{
    auto result=real<decltype(&eglGetProcAddress)>(GetProcAddress)(name);
    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(ReaObserveGraphicsSymbol(name,reinterpret_cast<void*>(result)));
}
EGLBoolean swapBuffers(EGLDisplay d,EGLSurface s)
{
    auto result=real<decltype(&eglSwapBuffers)>(SwapBuffers)(d,s);
    static std::atomic<unsigned> frames{0};if(frames.fetch_add(1)<5)TRACE("eglSwapBuffers display=%{public}p surface=%{public}p result=%{public}u",d,s,result);return result;
}
struct Binding { const char *name;Slot slot;void *wrapper; };
const Binding bindings[]={
#define ENTRY(name,slot,wrapper) {name,slot,reinterpret_cast<void*>(&wrapper)}
    ENTRY("eglGetDisplay",GetDisplay,getDisplay),ENTRY("eglInitialize",Initialize,initialize),
    ENTRY("eglChooseConfig",ChooseConfig,chooseConfig),ENTRY("eglBindAPI",BindAPI,bindAPI),
    ENTRY("eglCreateContext",CreateContext,createContext),ENTRY("eglCreateWindowSurface",CreateWindowSurface,createWindowSurface),
    ENTRY("eglCreatePbufferSurface",CreatePbufferSurface,createPbufferSurface),ENTRY("eglMakeCurrent",MakeCurrent,makeCurrent),
    ENTRY("eglGetError",GetError,getError),ENTRY("eglGetProcAddress",GetProcAddress,getProcAddress),
    ENTRY("glGetString",GetString,getString),ENTRY("eglSwapBuffers",SwapBuffers,swapBuffers)
#undef ENTRY
};
}
extern "C" void *ReaObserveGraphicsSymbol(const char *name, void *value)
{
    if(!name||!value)return value;
    for(const auto &binding:bindings)if(strcmp(name,binding.name)==0) {
        if(value==binding.wrapper)return value;
        originals[binding.slot].store(value,std::memory_order_release);
        Dl_info info{};dladdr(value,&info);
        TRACE("GRAPHICS_RESOLVE %{public}s actual=%{public}p library=%{public}s",name,value,info.dli_fname?info.dli_fname:"?");
        return binding.wrapper;
    }
    return value;
}
