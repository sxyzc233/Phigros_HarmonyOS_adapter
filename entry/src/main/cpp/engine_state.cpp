// Read-only original managed/native state, only on the Unity host thread.
#include <dlfcn.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <hilog/log.h>
extern "C" void ReaLogSyncState();
extern "C" void ReaLogJniState();

extern "C" void ReaLogEntryState(){
    ReaLogSyncState();
    ReaLogJniState();
    static void *library=dlopen("libil2cpp.so",RTLD_NOW|RTLD_NOLOAD);
    if(!library)return;
    auto domain=(void *(*)())dlsym(library,"il2cpp_domain_get");
    auto assemblies=(const void **(*)(void *,size_t *))dlsym(library,"il2cpp_domain_get_assemblies");
    auto image=(void *(*)(const void *))dlsym(library,"il2cpp_assembly_get_image");
    auto imageName=(const char *(*)(void *))dlsym(library,"il2cpp_image_get_name");
    auto findClass=(void *(*)(void *,const char *,const char *))dlsym(library,"il2cpp_class_from_name");
    auto findField=(void *(*)(void *,const char *))dlsym(library,"il2cpp_class_get_field_from_name");
    auto staticValue=(void (*)(void *,void *))dlsym(library,"il2cpp_field_static_get_value");
    auto fieldValue=(void (*)(void *,void *,void *))dlsym(library,"il2cpp_field_get_value");
    auto objectClass=(void *(*)(void *))dlsym(library,"il2cpp_object_get_class");
    auto findMethod=(void *(*)(void *,const char *,int))dlsym(library,"il2cpp_class_get_method_from_name");
    auto invoke=(void *(*)(void *,void *,void **,void **))dlsym(library,"il2cpp_runtime_invoke");
    auto unbox=(void *(*)(void *))dlsym(library,"il2cpp_object_unbox");
    auto stringLength=(int (*)(void *))dlsym(library,"il2cpp_string_length");
    auto stringChars=(const char16_t *(*)(void *))dlsym(library,"il2cpp_string_chars");
    auto resolve=(void *(*)(const char *))dlsym(library,"il2cpp_resolve_icall");
    if(!domain||!assemblies||!image||!imageName||!findClass||!findField||!staticValue||!fieldValue||!resolve)return;
    auto *d=domain();if(!d)return;
    size_t count=0;auto **list=assemblies(d,&count);void *game=nullptr;
    for(size_t i=0;i<count;++i){void *candidate=image(list[i]);const char *name=imageName(candidate);if(name&&strcmp(name,"Assembly-CSharp.dll")==0){game=candidate;break;}}
    int allowed=-1,startLock=-1;void *instance=nullptr;
    if(game){void *klass=findClass(game,"","SplashScene");if(klass){
        void *field=findField(klass,"instance");if(field)staticValue(field,&instance);
        if(instance){field=findField(klass,"allowStart");uint8_t value=0;if(field){fieldValue(instance,field,&value);allowed=value;}
            field=findField(klass,"gameStartLock");if(field)fieldValue(instance,field,&startLock);
        }
    }}
    auto touches=(int (*)())resolve("UnityEngine.Input::get_touchCount");
    auto mouse=(bool (*)(int))resolve("UnityEngine.Input::GetMouseButtonDown");
    auto width=(int (*)())resolve("UnityEngine.Screen::get_width");
    auto height=(int (*)())resolve("UnityEngine.Screen::get_height");
    struct Vec3 {float x,y,z;};Vec3 point{};
    auto position=(void (*)(Vec3 *))resolve("UnityEngine.Input::get_mousePosition_Injected");if(position)position(&point);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransState","ENTRY_STATE splash=%{public}d allowStart=%{public}d gameStartLock=%{public}d touches=%{public}d mouseDown=%{public}d screen=%{public}dx%{public}d mouse=(%{public}.1f,%{public}.1f)",instance?1:0,allowed,startLock,touches?touches():-1,mouse?mouse(0):-1,width?width():-1,height?height():-1,point.x,point.y);
    if(!instance||!objectClass||!findMethod||!invoke||!unbox||!stringLength||!stringChars)return;
    auto get=[&](void *object,const char *name)->void *{
        if(!object||strncmp(name,"get_",4)!=0)return nullptr;
        void *method=findMethod(objectClass(object),name,0);if(!method)return nullptr;
        void *exception=nullptr;void *result=invoke(method,object,nullptr,&exception);
        if(exception){OH_LOG_Print(LOG_APP,LOG_WARN,0,"TransState","STATE_GETTER_EXCEPTION method=%{public}s",name);return nullptr;}
        return result;
    };
    auto ref=[&](void *object,const char *name)->void *{
        if(!object)return nullptr;void *out=nullptr;
        void *field=findField(objectClass(object),name);if(field)fieldValue(object,field,&out);return out;
    };
    auto boolean=[&](void *object,const char *name)->int{void *boxed=get(object,name);return boxed?*static_cast<uint8_t *>(unbox(boxed)):-1;};
    auto shortName=[&](void *object,char *out,size_t capacity){
        void *name=get(object,"get_name");size_t length=name?stringLength(name):0;
        const char16_t *text=name?stringChars(name):nullptr;size_t i=0;
        for(;text&&i<length&&i+1<capacity;++i)out[i]=text[i]<128?static_cast<char>(text[i]):'?';out[i]=0;
    };
    auto node=[&](const char *label,void *component){
        void *go=get(component,"get_gameObject");char name[80]{};shortName(go,name,sizeof(name));
        void *rect=get(component,"get_rect"),*scale=get(component,"get_localScale"),*position=get(component,"get_position");
        float bounds[4]{},size[3]{},where[3]{};
        if(rect)memcpy(bounds,unbox(rect),sizeof(bounds));
        if(scale)memcpy(size,unbox(scale),sizeof(size));
        if(position)memcpy(where,unbox(position),sizeof(where));
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransState","UI_NODE label=%{public}s exists=%{public}d name=%{public}s self=%{public}d hierarchy=%{public}d enabled=%{public}d rect=(%{public}.1f,%{public}.1f,%{public}.1f,%{public}.1f) scale=(%{public}.2f,%{public}.2f,%{public}.2f) world=(%{public}.1f,%{public}.1f,%{public}.1f)",label,component?1:0,name,boolean(go,"get_activeSelf"),boolean(go,"get_activeInHierarchy"),boolean(component,"get_enabled"),bounds[0],bounds[1],bounds[2],bounds[3],size[0],size[1],size[2],where[0],where[1],where[2]);
    };
    void *privacy=ref(instance,"privacyPolicy");uint8_t showing=0,okay=0;int delay=-1;
    if(privacy){
        void *klass=objectClass(privacy),*field=findField(klass,"isShowing");if(field)fieldValue(privacy,field,&showing);
        field=findField(klass,"<IsOk>k__BackingField");if(field)fieldValue(privacy,field,&okay);
        field=findField(klass,"<ConfirmationDelay>k__BackingField");if(field)fieldValue(privacy,field,&delay);
    }
    void *title=get(ref(privacy,"title"),"get_text"),*content=get(ref(privacy,"content"),"get_text");
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransState","PRIVACY_STATE exists=%{public}d showing=%{public}d okay=%{public}d delay=%{public}d titleLength=%{public}d contentLength=%{public}d",privacy?1:0,showing,okay,delay,title?stringLength(title):-1,content?stringLength(content):-1);
    node("privacy",privacy);node("box",ref(privacy,"box"));node("title",ref(privacy,"title"));node("content",ref(privacy,"content"));
    void *button=ref(privacy,"okButton");node("ok",button);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransState","PRIVACY_OK interactable=%{public}d",boolean(button,"get_interactable"));
    void *parent=get(ref(privacy,"box"),"get_parent");
    for(int i=0;parent&&i<5;++i){char label[32];snprintf(label,sizeof(label),"box-parent-%d",i);node(label,parent);parent=get(parent,"get_parent");}
}
