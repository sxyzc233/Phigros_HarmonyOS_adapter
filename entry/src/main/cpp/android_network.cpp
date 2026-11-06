// Android bionic netdb ABI adapter. Android addrinfo has canonname at 24 and
// sockaddr at 32; HarmonyOS musl uses the opposite order. Flag/error enums differ.
// Sources: AOSP bionic libc/include/netdb.h; installed HarmonyOS SDK netdb.h.
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <atomic>
#include <cerrno>
#include <hilog/log.h>

struct AndroidAddrInfo {
    int flags,family,socktype,protocol;
    socklen_t addrlen;
    char *canonname;
    sockaddr *addr;
    AndroidAddrInfo *next;
};
static_assert(offsetof(AndroidAddrInfo,canonname)==24&&offsetof(AndroidAddrInfo,addr)==32&&sizeof(AndroidAddrInfo)==48,"bionic LP64 addrinfo");
namespace {
int NativeFlags(int flags){
    const int known=1|2|4|8|0x100|0x200|0x400|0x800;
    if(flags&~known)return -1;
    int value=flags&7;
    if(flags&8)value|=AI_NUMERICSERV;
    if(flags&0x100)value|=AI_ALL;
    if(flags&0x400)value|=AI_ADDRCONFIG;
    if(flags&(0x200|0x800))value|=AI_V4MAPPED;
    return value;
}
int ForeignError(int e){
    if(!e)return 0;
    switch(e){
    case EAI_AGAIN:return 2;case EAI_BADFLAGS:return 3;case EAI_FAIL:return 4;
    case EAI_FAMILY:return 5;case EAI_MEMORY:return 6;case EAI_NONAME:return 8;
    case EAI_SERVICE:return 9;case EAI_SOCKTYPE:return 10;case EAI_SYSTEM:return 11;
    case EAI_OVERFLOW:return 14;default:return 4;
    }
}
int ForeignFlags(int flags){
    int out=flags&7;
    if(flags&AI_NUMERICSERV)out|=8;if(flags&AI_ALL)out|=0x100;
    if(flags&AI_ADDRCONFIG)out|=0x400;if(flags&AI_V4MAPPED)out|=0x800;return out;
}
}
extern "C" void rea_freeaddrinfo(AndroidAddrInfo *entry){
    while(entry){auto *next=entry->next;free(entry->canonname);free(entry->addr);free(entry);entry=next;}
}
extern "C" int rea_getaddrinfo(const char *node,const char *service,const AndroidAddrInfo *foreign,AndroidAddrInfo **out){
    if(!out)return 4;*out=nullptr;
    addrinfo hints{},*native=nullptr;
    if(foreign){
        hints.ai_flags=NativeFlags(foreign->flags);
        if(hints.ai_flags<0)return 3;
        hints.ai_family=foreign->family;hints.ai_socktype=foreign->socktype;hints.ai_protocol=foreign->protocol;
    }
    int result=getaddrinfo(node,service,foreign?&hints:nullptr,&native);
    static std::atomic<int> calls{0};
    if(calls.fetch_add(1)<96)OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransNetwork","DNS host=%{public}s service=%{public}s nativeResult=%{public}d",node?node:"(null)",service?service:"(null)",result);
    if(result)return ForeignError(result);
    AndroidAddrInfo **tail=out;
    for(auto *p=native;p;p=p->ai_next){
        auto *copy=static_cast<AndroidAddrInfo *>(calloc(1,sizeof(AndroidAddrInfo)));
        if(!copy){result=EAI_MEMORY;break;}
        copy->flags=ForeignFlags(p->ai_flags);copy->family=p->ai_family;copy->socktype=p->ai_socktype;copy->protocol=p->ai_protocol;copy->addrlen=p->ai_addrlen;
        if(p->ai_addr&&p->ai_addrlen){copy->addr=static_cast<sockaddr *>(malloc(p->ai_addrlen));if(copy->addr)memcpy(copy->addr,p->ai_addr,p->ai_addrlen);else result=EAI_MEMORY;}
        if(p->ai_canonname){copy->canonname=strdup(p->ai_canonname);if(!copy->canonname)result=EAI_MEMORY;}
        *tail=copy;tail=&copy->next;if(result)break;
    }
    freeaddrinfo(native);
    if(result){rea_freeaddrinfo(*out);*out=nullptr;return ForeignError(result);}return 0;
}
extern "C" const char *rea_gai_strerror(int error){
    switch(error){case 0:return "Success";case 1:return "Address family for hostname not supported";
    case 2:return gai_strerror(EAI_AGAIN);case 3:return gai_strerror(EAI_BADFLAGS);case 4:return gai_strerror(EAI_FAIL);
    case 5:return gai_strerror(EAI_FAMILY);case 6:return gai_strerror(EAI_MEMORY);case 7:case 8:return gai_strerror(EAI_NONAME);
    case 9:return gai_strerror(EAI_SERVICE);case 10:return gai_strerror(EAI_SOCKTYPE);case 11:return gai_strerror(EAI_SYSTEM);
    case 14:return gai_strerror(EAI_OVERFLOW);default:return "Unknown Android resolver error";}
}
extern "C" int rea_getnameinfo(const sockaddr *address,socklen_t length,char *host,size_t hostSize,char *service,size_t serviceSize,int flags){
    if(flags&~31)return 3;int native=0;
    if(flags&1)native|=NI_NOFQDN;if(flags&2)native|=NI_NUMERICHOST;if(flags&4)native|=NI_NAMEREQD;
    if(flags&8)native|=NI_NUMERICSERV;if(flags&16)native|=NI_DGRAM;
    if(hostSize>0xffffffff||serviceSize>0xffffffff)return 14;
    return ForeignError(getnameinfo(address,length,host,hostSize,service,serviceSize,native));
}
extern "C" int rea_net_connect(int fd,const sockaddr *address,socklen_t length){
    int result=connect(fd,address,length),saved=errno;
    static std::atomic<int> calls{0};
    if(calls.fetch_add(1)<64){
        char ip[INET6_ADDRSTRLEN]="?";unsigned port=0;
        if(address&&address->sa_family==AF_INET&&length>=sizeof(sockaddr_in)){auto *a=reinterpret_cast<const sockaddr_in *>(address);inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip));port=ntohs(a->sin_port);}
        if(address&&address->sa_family==AF_INET6&&length>=sizeof(sockaddr_in6)){auto *a=reinterpret_cast<const sockaddr_in6 *>(address);inet_ntop(AF_INET6,&a->sin6_addr,ip,sizeof(ip));port=ntohs(a->sin6_port);}
        OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransNetwork","CONNECT ip=%{public}s port=%{public}u result=%{public}d errno=%{public}d",ip,port,result,saved);
    }
    errno=saved;return result;
}
extern "C" int ReaNetworkSelfTest(){
    int failed=0,checks=0;
    auto check=[&](bool ok){++checks;if(!ok)++failed;OH_LOG_Print(LOG_APP,ok?LOG_INFO:LOG_ERROR,0,"TransNetwork","NETWORK_CHECK index=%{public}d result=%{public}s",checks,ok?"PASS":"FAIL");};
    AndroidAddrInfo hints{},*out=nullptr;hints.flags=4|8;hints.family=AF_INET;hints.socktype=SOCK_STREAM;
    int result=rea_getaddrinfo("127.0.0.1","443",&hints,&out);check(result==0&&out&&out->addr);
    check(out&&out->addrlen==sizeof(sockaddr_in)&&out->addr->sa_family==AF_INET);
    char host[64]{},service[16]{};
    check(out&&rea_getnameinfo(out->addr,out->addrlen,host,sizeof(host),service,sizeof(service),2|8)==0&&strcmp(host,"127.0.0.1")==0&&strcmp(service,"443")==0);
    rea_freeaddrinfo(out);out=nullptr;
    hints.family=AF_INET6;result=rea_getaddrinfo("::1","80",&hints,&out);
    check(result==0&&out&&out->addr&&out->addr->sa_family==AF_INET6);rea_freeaddrinfo(out);out=nullptr;
    check(rea_getaddrinfo("not-an-ip","443",&hints,&out)==8&&!out);
    hints.flags=0x40000000;check(rea_getaddrinfo("127.0.0.1","80",&hints,&out)==3&&!out);
    check(NativeFlags(0x400)==AI_ADDRCONFIG&&NativeFlags(8)==AI_NUMERICSERV&&NativeFlags(0x800)==AI_V4MAPPED);
    OH_LOG_Print(LOG_APP,failed?LOG_ERROR:LOG_INFO,0,"TransNetwork","NETWORK_ABI_SELFTEST %{public}s checks=%{public}d failed=%{public}d",failed?"FAIL":"PASS",checks,failed);
    return failed;
}
