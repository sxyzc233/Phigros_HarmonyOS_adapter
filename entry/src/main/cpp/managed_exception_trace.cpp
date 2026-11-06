// Observe managed exceptions at their existing C++ ABI boundary. No exception
// is swallowed or changed; never install a signal handler or inspect payloads.
#include <dlfcn.h>
#include <typeinfo>
#include <cstring>
#include <atomic>
#include <cstdint>
#include <execinfo.h>
#include <hilog/log.h>

namespace {
using Throw=void (*)(void *,void *,void (*)(void *));
Throw realThrow=nullptr;
void *(*objectClass)(void *)=nullptr;
const char *(*className)(void *)=nullptr;
const char *(*classNamespace)(void *)=nullptr;
std::atomic<unsigned> count{0};
thread_local bool tracing=false;

[[noreturn]] void ObserveThrow(void *object,void *type,void (*destroy)(void *)){
    const char *cppType=type?static_cast<std::type_info *>(type)->name():nullptr;
    if(!tracing&&object&&objectClass&&className&&classNamespace&&cppType&&strstr(cppType,"Il2CppExceptionWrapper")){
        tracing=true;
        unsigned serial=count.fetch_add(1,std::memory_order_relaxed)+1;
        if(serial<=128){
            void *exception=*static_cast<void **>(object);
            void *klass=exception?objectClass(exception):nullptr;
            OH_LOG_Print(LOG_APP,LOG_WARN,0,"TransException","MANAGED_THROW seq=%{public}u type=%{public}s.%{public}s (message and data withheld)",serial,
                klass?classNamespace(klass):"?",klass?className(klass):"?");
            void *frames[16];int n=backtrace(frames,16);
            for(int i=1;i<n;++i){Dl_info info{};if(dladdr(frames[i],&info)&&info.dli_fbase){
                const char *module=info.dli_fname?strrchr(info.dli_fname,'/'):nullptr;
                OH_LOG_Print(LOG_APP,LOG_WARN,0,"TransException","MANAGED_FRAME seq=%{public}u index=%{public}d image=%{public}s offset=%{public}lx",serial,i,
                    module?module+1:"?",(unsigned long)((uintptr_t)frames[i]-(uintptr_t)info.dli_fbase));
            }}
        }
        tracing=false;
    }
    realThrow(object,type,destroy);
    __builtin_unreachable();
}
}

extern "C" void *ReaExceptionThrowHook(){
    if(!objectClass||!className||!classNamespace)return nullptr;
    if(!realThrow)realThrow=reinterpret_cast<Throw>(dlsym(RTLD_DEFAULT,"__cxa_throw"));
    return realThrow?reinterpret_cast<void *>(&ObserveThrow):nullptr;
}
extern "C" void ReaConfigureExceptionTrace(void *library){
    objectClass=reinterpret_cast<decltype(objectClass)>(dlsym(library,"il2cpp_object_get_class"));
    className=reinterpret_cast<decltype(className)>(dlsym(library,"il2cpp_class_get_name"));
    classNamespace=reinterpret_cast<decltype(classNamespace)>(dlsym(library,"il2cpp_class_get_namespace"));
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransException","MANAGED_TRACE_READY result=%{public}d",objectClass&&className&&classNamespace?1:0);
}
