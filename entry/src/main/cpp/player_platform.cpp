#include "player_platform.h"
#include "host_lifecycle.h"
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <hilog/log.h>

namespace {
std::mutex keyboardMutex;
ReaKeyboardState keyboard;
std::vector<ReaKeyboardEvent> events;
std::atomic<int64_t> lastFrame{0}, period{16666667};
std::atomic<double> refresh{60};
std::atomic<bool> exitRequested{false};
std::mutex dialogMutex;
ReaDialogState dialog;
std::vector<ReaDialogResult> dialogResults;
int64_t Now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000000000+t.tv_nsec;}
std::string String(napi_env e,napi_value v){size_t n=0;if(napi_get_value_string_utf8(e,v,nullptr,0,&n)!=napi_ok||n>1024*1024)return {};std::string s(n+1,'\0');napi_get_value_string_utf8(e,v,&s[0],s.size(),&n);s.resize(n);return s;}
void Number(napi_env e,napi_value o,const char *k,double x){napi_value v;napi_create_double(e,x,&v);napi_set_named_property(e,o,k,v);}
void Boolean(napi_env e,napi_value o,const char *k,bool x){napi_value v;napi_get_boolean(e,x,&v);napi_set_named_property(e,o,k,v);}
void Text(napi_env e,napi_value o,const char *k,const std::string &x){napi_value v;napi_create_string_utf8(e,x.data(),x.size(),&v);napi_set_named_property(e,o,k,v);}
napi_value ReadKeyboard(napi_env e,napi_callback_info info){
    size_t n=1;napi_value a;napi_get_cb_info(e,info,&n,&a,nullptr,nullptr);uint32_t revision=0;if(n)napi_get_value_uint32(e,a,&revision);
    std::lock_guard<std::mutex> lock(keyboardMutex);napi_value o;
    if(revision==keyboard.revision){napi_get_null(e,&o);return o;}
    napi_create_object(e,&o);Number(e,o,"revision",keyboard.revision);Number(e,o,"session",keyboard.session);
    Boolean(e,o,"visible",keyboard.visible);Boolean(e,o,"secure",keyboard.secure);Boolean(e,o,"multiline",keyboard.multiline);
    Number(e,o,"type",keyboard.type);Number(e,o,"limit",keyboard.limit);Number(e,o,"start",keyboard.start);Number(e,o,"length",keyboard.length);
    Text(e,o,"text",keyboard.text);Text(e,o,"hint",keyboard.hint);return o;
}
napi_value ReplyKeyboard(napi_env e,napi_callback_info info){
    size_t n=5;napi_value a[5];napi_get_cb_info(e,info,&n,a,nullptr,nullptr);ReaKeyboardEvent event;bool accepted=false;
    if(n==5&&napi_get_value_uint32(e,a[0],&event.session)==napi_ok&&napi_get_value_int32(e,a[2],&event.start)==napi_ok&&
       napi_get_value_int32(e,a[3],&event.length)==napi_ok&&napi_get_value_int32(e,a[4],&event.status)==napi_ok&&event.status>=0&&event.status<=2){
        event.text=String(e,a[1]);std::lock_guard<std::mutex> lock(keyboardMutex);
        if(event.session==keyboard.session&&keyboard.visible){
            // Coalesce edits only. Never drop or reorder submit/cancel.
            if(!events.empty()&&events.back().session==event.session&&events.back().status==0&&event.status==0)events.back()=event;
            else events.push_back(event);
            keyboard.text=event.text;keyboard.start=event.start;keyboard.length=event.length;
            if(event.status){keyboard.visible=false;++keyboard.revision;}
            accepted=true;
        }
    }
    napi_value v;napi_get_boolean(e,accepted,&v);return v;
}
napi_value DisplayRate(napi_env e,napi_callback_info info){
    size_t n=1;napi_value a;napi_get_cb_info(e,info,&n,&a,nullptr,nullptr);double hz=0;
    if(n&&napi_get_value_double(e,a,&hz)==napi_ok&&std::isfinite(hz)&&hz>=24&&hz<=240){refresh=hz;period=int64_t(1e9/hz);}
    napi_value v;napi_get_undefined(e,&v);return v;
}
napi_value ExitRequested(napi_env e,napi_callback_info){napi_value v;napi_get_boolean(e,exitRequested.load(),&v);return v;}
napi_value AppForeground(napi_env e,napi_callback_info info){
    size_t n=1;napi_value a;bool foreground=false;napi_get_cb_info(e,info,&n,&a,nullptr,nullptr);
    if(n&&napi_get_value_bool(e,a,&foreground)==napi_ok)ReaSetAppForeground(foreground);
    napi_value v;napi_get_undefined(e,&v);return v;
}
napi_value SurfaceAvailable(napi_env e,napi_callback_info){napi_value v;napi_get_boolean(e,ReaNativeSurfaceAvailable(),&v);return v;}
napi_value ClearSurface(napi_env e,napi_callback_info info){
    size_t n=1;napi_value a;napi_get_cb_info(e,info,&n,&a,nullptr,nullptr);
    if(n){auto id=String(e,a);ReaClearSurfaceId(id.c_str());}
    napi_value v;napi_get_undefined(e,&v);return v;
}
napi_value ReadDialog(napi_env e,napi_callback_info info){
    size_t n=1;napi_value a;napi_get_cb_info(e,info,&n,&a,nullptr,nullptr);uint32_t revision=0;if(n)napi_get_value_uint32(e,a,&revision);
    std::lock_guard<std::mutex> lock(dialogMutex);napi_value o;if(revision==dialog.revision){napi_get_null(e,&o);return o;}
    napi_create_object(e,&o);Number(e,o,"revision",dialog.revision);Number(e,o,"session",dialog.session);Number(e,o,"kind",dialog.kind);
    Boolean(e,o,"visible",dialog.visible);Boolean(e,o,"cancelable",dialog.cancelable);Text(e,o,"title",dialog.title);Text(e,o,"message",dialog.message);
    napi_value buttons;napi_create_array_with_length(e,dialog.buttons.size(),&buttons);
    for(size_t i=0;i<dialog.buttons.size();++i){napi_value text;napi_create_string_utf8(e,dialog.buttons[i].data(),dialog.buttons[i].size(),&text);napi_set_element(e,buttons,i,text);}
    napi_set_named_property(e,o,"buttons",buttons);return o;
}
napi_value ReplyDialog(napi_env e,napi_callback_info info){
    size_t n=2;napi_value a[2];napi_get_cb_info(e,info,&n,a,nullptr,nullptr);uint32_t session=0;int index=-2;bool okay=false;
    if(n==2&&napi_get_value_uint32(e,a[0],&session)==napi_ok&&napi_get_value_int32(e,a[1],&index)==napi_ok){
        std::lock_guard<std::mutex> lock(dialogMutex);
        if(session==dialog.session&&dialog.visible&&((index==-1&&dialog.cancelable)||(index>=0&&size_t(index)<dialog.buttons.size()))){
            if(index>=0){
                const char *targets[]={"MobileDialogInfo","MobileDialogConfirm","MobileDialogNeutral"};
                const char *confirm[]={"OnYesCallBack","OnNoCallBack"};
                const char *neutral[]={"OnAcceptCallBack","OnNeutralCallBack","OnDeclineCallBack"};
                const char *method=dialog.kind==0?"OnOkCallBack":dialog.kind==1?confirm[index]:neutral[index];
                dialogResults.push_back({targets[dialog.kind],method,std::to_string(index)});
            }
            dialog.visible=false;++dialog.revision;okay=true;
            OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransInput","NATIVE_DIALOG_REPLY session=%{public}u index=%{public}d",session,index);
        }
    }
    napi_value v;napi_get_boolean(e,okay,&v);return v;
}
napi_value FinishExit(napi_env e,napi_callback_info){
    // Only the original player's explicit quit/kill can authorize this path.
    if(exitRequested.load())_exit(0);
    napi_value v;napi_get_undefined(e,&v);return v;
}
}
void ReaRequestPlayerExit(const char *reason){
    if(!exitRequested.exchange(true))OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransInput","PLAYER_EXIT_REQUEST source=%{public}s",reason);
}
void ReaShowNativeDialog(const ReaDialogState &state){
    std::lock_guard<std::mutex> lock(dialogMutex);auto revision=dialog.revision+1,session=dialog.session+1;
    dialog=state;dialog.revision=revision;dialog.session=session;dialog.visible=true;
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransInput","NATIVE_DIALOG_SHOW kind=%{public}d session=%{public}u",dialog.kind,session);
}
void ReaDismissNativeDialog(){std::lock_guard<std::mutex> lock(dialogMutex);dialog.visible=false;++dialog.revision;}
std::vector<ReaDialogResult> ReaTakeDialogResults(){std::lock_guard<std::mutex> lock(dialogMutex);std::vector<ReaDialogResult> r;r.swap(dialogResults);return r;}
void ReaShowKeyboard(const ReaKeyboardState &state){
    std::lock_guard<std::mutex> lock(keyboardMutex);uint32_t revision=keyboard.revision+1,session=keyboard.session+1;
    keyboard=state;keyboard.visible=true;keyboard.revision=revision;keyboard.session=session;
    events.push_back({session,3,0,0,{}});
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransInput","IME_SHOW session=%{public}u type=%{public}d secure=%{public}d",session,state.type,state.secure);
}
void ReaChangeKeyboard(const char *text,int limit,int start,int length){
    std::lock_guard<std::mutex> lock(keyboardMutex);if(text)keyboard.text=text;if(limit>=0)keyboard.limit=limit;
    if(start>=0){keyboard.start=start;keyboard.length=length;}++keyboard.revision;
}
void ReaHideKeyboard(){std::lock_guard<std::mutex> lock(keyboardMutex);keyboard.visible=false;++keyboard.revision;}
std::vector<ReaKeyboardEvent> ReaTakeKeyboardEvents(){std::lock_guard<std::mutex> lock(keyboardMutex);std::vector<ReaKeyboardEvent> out;out.swap(events);return out;}
void ReaExportPlatform(napi_env e,napi_value o){napi_property_descriptor d[]={
    {"setAppForeground",nullptr,AppForeground,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"nativeSurfaceAvailable",nullptr,SurfaceAvailable,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"clearSurface",nullptr,ClearSurface,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"keyboardState",nullptr,ReadKeyboard,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"keyboardReply",nullptr,ReplyKeyboard,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"setDisplayRefreshRate",nullptr,DisplayRate,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"exitRequested",nullptr,ExitRequested,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"finishRequestedExit",nullptr,FinishExit,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"dialogState",nullptr,ReadDialog,nullptr,nullptr,nullptr,napi_default,nullptr},
    {"dialogReply",nullptr,ReplyDialog,nullptr,nullptr,nullptr,napi_default,nullptr}};
    napi_define_properties(e,o,sizeof(d)/sizeof(d[0]),d);
}
extern "C" void ReaDisplayFrame(uint64_t timestamp,uint64_t target){
    (void)target;int64_t before=lastFrame.exchange(timestamp),delta=int64_t(timestamp)-before;
    if(before>0&&delta>4000000&&delta<50000000){
        // Real callback cadence, not a fabricated 120Hz device capability.
        int64_t p=period.load();if(std::llabs(delta-p)<p/5)period=(p*7+delta)/8;
    }
}
extern "C" double ReaDisplayRefreshRate(){return refresh.load();}
extern "C" int64_t ReaNextFrameDelayNs(int64_t minimum){
    int64_t p=period.load(),now=Now(),last=lastFrame.load(),wanted=now+std::max<int64_t>(minimum,0);
    if(last<=0||last>now+p||now-last>p*8)return std::max(minimum,p);
    int64_t next=last+((std::max<int64_t>(wanted-last,0)/p)+1)*p;return std::max<int64_t>(next-now,1);
}
