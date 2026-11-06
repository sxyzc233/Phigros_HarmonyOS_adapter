#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <hilog/log.h>
namespace {
struct Query {int androidValue,nativeValue;const char *name;};
static const Query queries[]={
#include "android_sysconf_map.inc"
};
}
extern "C" long rea_sysconf(int key){
    static std::atomic<int> count{0};
    for(const auto &query:queries)if(query.androidValue==key){
        long value=sysconf(query.nativeValue);int saved=errno;
        if(count.fetch_add(1)<48)OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransSystem","SYSCONF android=%{public}d native=%{public}d name=%{public}s value=%{public}ld",key,query.nativeValue,query.name,value);
        errno=saved;return value;
    }
    errno=EINVAL;return -1;
}
extern "C" int ReaSysconfSelfTest(){
    int checks=0,failed=0;
    auto check=[&](bool okay){++checks;if(!okay)++failed;};
    check(rea_sysconf(0x61)==sysconf(_SC_NPROCESSORS_ONLN)&&rea_sysconf(0x61)>0);
    check(rea_sysconf(0x60)==sysconf(_SC_NPROCESSORS_CONF)&&rea_sysconf(0x60)>0);
    check(rea_sysconf(0x27)==getpagesize()&&rea_sysconf(0x28)==getpagesize());
    check(rea_sysconf(0x06)==sysconf(_SC_CLK_TCK));
    check(rea_sysconf(0x62)==sysconf(_SC_PHYS_PAGES));
    errno=0;check(rea_sysconf(0x7fffffff)==-1&&errno==EINVAL);
    OH_LOG_Print(LOG_APP,failed?LOG_ERROR:LOG_INFO,0,"TransSystem","SYSCONF_ABI_SELFTEST %{public}s checks=%{public}d failed=%{public}d",failed?"FAIL":"PASS",checks,failed);
    return failed;
}
