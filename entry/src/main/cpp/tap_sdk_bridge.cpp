#include "tap_sdk_bridge.h"
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <hilog/log.h>

namespace {
std::mutex mutex;
uint32_t serial = 0;
std::vector<ReaSdkRequest> requests;
std::unordered_map<uint32_t, void *> callbacks;
struct Reply { void *proxy; std::string payload; uint32_t id; };
std::vector<Reply> replies;
constexpr size_t kPayloadLimit = 262144;
}

bool ReaSdkEnqueue(const std::string &command, void *proxy) {
    std::lock_guard<std::mutex> lock(mutex);
    if(command.empty() || command.size()>kPayloadLimit || requests.size()>=64 || callbacks.size()>=128) return false;
    uint32_t id=++serial;
    if(proxy) callbacks[id]=proxy;
    requests.push_back({id,command});
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransSdk","SDK_REQUEST id=%{public}u callback=%{public}d",id,proxy?1:0);
    return true;
}

std::vector<ReaSdkRequest> ReaSdkTakeRequests() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<ReaSdkRequest> batch;batch.swap(requests);return batch;
}

bool ReaSdkReply(uint32_t id,const std::string &result,bool finished) {
    std::lock_guard<std::mutex> lock(mutex);
    auto found=callbacks.find(id);
    if(found==callbacks.end() || result.empty() || result.size()>kPayloadLimit || replies.size()>=64) return false;
    replies.push_back({found->second,result,id});
    if(finished) callbacks.erase(found);
    return true;
}

void ReaSdkRelease(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    callbacks.erase(id);
    replies.erase(std::remove_if(replies.begin(),replies.end(),[id](const Reply &r){return r.id==id;}),replies.end());
    requests.erase(std::remove_if(requests.begin(),requests.end(),[id](const ReaSdkRequest &r){return r.id==id;}),requests.end());
}

void ReaSdkDrainReplies() {
    std::vector<Reply> batch;
    {std::lock_guard<std::mutex> lock(mutex);batch.swap(replies);}
    for(const auto &reply:batch) {
        bool ok=ReaDeliverSdkCallback(reply.proxy,reply.payload.c_str());
        OH_LOG_Print(LOG_APP,ok?LOG_INFO:LOG_ERROR,0,"TransSdk","SDK_DELIVER id=%{public}u result=%{public}d",reply.id,ok?1:0);
    }
}
