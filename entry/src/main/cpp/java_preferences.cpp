// Durable Android SharedPreferences backing; no game keys, decryption or progress rewriting.
#include "java_preferences.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <chrono>
#include <hilog/log.h>

namespace rea_prefs {
namespace {
const char *root="/data/storage/el2/base/files/rea-jprefs";
const size_t limit=64*1024*1024;
std::mutex registryMutex;
std::map<std::string,std::shared_ptr<Store>> registry;
std::atomic<unsigned> serial{0};
void u32(std::vector<uint8_t> &out,uint32_t value){for(int i=0;i<4;++i)out.push_back(value>>(i*8));}
void u64(std::vector<uint8_t> &out,uint64_t value){for(int i=0;i<8;++i)out.push_back(value>>(i*8));}
void str(std::vector<uint8_t> &out,const std::string &value){u32(out,value.size());out.insert(out.end(),value.begin(),value.end());}
struct Reader {
    const std::vector<uint8_t> &data;size_t at=0;bool ok=true;
    uint64_t number(size_t n){if(!ok||at+n>data.size()){ok=false;return 0;}uint64_t value=0;for(size_t i=0;i<n;++i)value|=(uint64_t)data[at++]<<(i*8);return value;}
    std::string string(){size_t n=number(4);if(!ok||n>16*1024*1024||at+n>data.size()){ok=false;return {};}
        std::string value(reinterpret_cast<const char *>(data.data()+at),n);at+=n;return value;}
};
std::string pathFor(const char *directory,const std::string &name){
    uint64_t hash=14695981039346656037ULL;for(unsigned char byte:name){hash^=byte;hash*=1099511628211ULL;}
    char base[40];snprintf(base,sizeof(base),"/%016llx.prefs",(unsigned long long)hash);return std::string(directory)+base;
}
bool load(const std::string &path,const std::string &name,Values &values,bool &exists){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);exists=fd>=0||errno!=ENOENT;if(fd<0)return false;
    struct stat status{};if(fstat(fd,&status)||status.st_size<24||(uint64_t)status.st_size>limit){close(fd);return false;}
    std::vector<uint8_t> bytes(status.st_size);size_t done=0;
    while(done<bytes.size()){ssize_t n=read(fd,bytes.data()+done,bytes.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return false;}done+=n;}close(fd);
    static const uint8_t magic[12]={'R','E','A','_','J','P','R','E','F','S',1,0};
    if(memcmp(bytes.data(),magic,sizeof(magic)))return false;
    uint32_t expected=0;for(int i=0;i<4;++i)expected|=(uint32_t)bytes[bytes.size()-4+i]<<(i*8);
    if(crc32(0,bytes.data(),bytes.size()-4)!=expected)return false;
    Reader reader{bytes};reader.at=12;if(reader.string()!=name)return false;uint32_t count=reader.number(4);if(count>200000)return false;
    Values decoded;
    for(uint32_t i=0;i<count&&reader.ok;++i){std::string key=reader.string();Value value;value.type=(Type)reader.number(1);
        if(value.type==Type::String)value.text=reader.string();
        else if(value.type==Type::Float){uint32_t bits=reader.number(4);memcpy(&value.number,&bits,4);}
        else if(value.type==Type::Int||value.type==Type::Long||value.type==Type::Bool)value.integer=reader.number(8);
        else reader.ok=false;
        if(!decoded.emplace(key,std::move(value)).second)reader.ok=false;
    }
    if(!reader.ok||reader.at!=bytes.size()-4)return false;
    values.swap(decoded);return true;
}
std::shared_ptr<Store> openStore(const char *directory,const std::string &name){
    if(name.size()>4096)return nullptr;
    if(mkdir(directory,0700)!=0&&errno!=EEXIST)return nullptr;
    auto store=std::make_shared<Store>();store->name=name;store->path=pathFor(directory,name);
    bool mainExists=false,backupExists=false;
    if(load(store->path,name,store->values,mainExists))return store;
    if(load(store->path+".bak",name,store->values,backupExists)){
        store->mainValid=false;OH_LOG_Print(LOG_APP,LOG_WARN,0,"TransPrefs","PREFS_RECOVERED_BACKUP entries=%{public}zu",store->values.size());return store;
    }
    if(mainExists||backupExists){OH_LOG_Print(LOG_APP,LOG_ERROR,0,"TransPrefs","PREFS_CORRUPT refusing empty replacement");return nullptr;}
    return store;
}
bool copySynced(const std::string &source,const std::string &destination){
    int input=open(source.c_str(),O_RDONLY|O_CLOEXEC);if(input<0)return false;
    int output=open(destination.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(output<0){close(input);return false;}
    uint8_t buffer[16384];size_t total=0;bool okay=true;
    while(okay){ssize_t count=read(input,buffer,sizeof(buffer));if(count<0&&errno==EINTR)continue;
        if(count<0){okay=false;break;}if(!count)break;total+=count;if(total>limit){okay=false;break;}
        size_t done=0;while(done<(size_t)count){ssize_t n=write(output,buffer+done,count-done);if(n<0&&errno==EINTR)continue;if(n<=0){okay=false;break;}done+=n;}
    }
    close(input);if(okay&&fsync(output))okay=false;if(close(output))okay=false;
    if(!okay)unlink(destination.c_str());return okay;
}
bool save(Store &store,const Values &values){
    const auto began=std::chrono::steady_clock::now();
    if(values.size()>200000)return false;
    static const uint8_t magic[12]={'R','E','A','_','J','P','R','E','F','S',1,0};
    std::vector<uint8_t> bytes(magic,magic+sizeof(magic));str(bytes,store.name);u32(bytes,values.size());
    for(const auto &entry:values){if(entry.first.size()>16*1024*1024||entry.second.text.size()>16*1024*1024)return false;
        str(bytes,entry.first);bytes.push_back((uint8_t)entry.second.type);
        if(entry.second.type==Type::String)str(bytes,entry.second.text);
        else if(entry.second.type==Type::Float){uint32_t bits;memcpy(&bits,&entry.second.number,4);u32(bytes,bits);}
        else u64(bytes,entry.second.integer);
        if(bytes.size()>limit-4)return false;
    }
    u32(bytes,crc32(0,bytes.data(),bytes.size()));
    std::string temporary=store.path+".tmp."+std::to_string(getpid())+"."+std::to_string(++serial);
    int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return false;
    size_t done=0;bool ok=true;
    while(done<bytes.size()){ssize_t n=write(fd,bytes.data()+done,bytes.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=n;}
    if(ok&&fsync(fd))ok=false;if(close(fd))ok=false;
    // Keep the previous complete file, not a partly written or corrupt generation.
    if(ok&&store.mainValid&&access(store.path.c_str(),F_OK)==0){
        std::string backupTemp=temporary+".bak";
        // Harmony denies hard links in this app's sandbox. Copy to a new private file,
        // fsync and validate it before replacing the previous backup.
        Values checked;bool present=false;
        if(!copySynced(store.path,backupTemp)||!load(backupTemp,store.name,checked,present)){unlink(backupTemp.c_str());ok=false;}
        else if(rename(backupTemp.c_str(),(store.path+".bak").c_str())!=0){unlink(backupTemp.c_str());ok=false;}
    }
    if(ok&&rename(temporary.c_str(),store.path.c_str()))ok=false;
    if(!ok){unlink(temporary.c_str());return false;}
    std::string directory=store.path.substr(0,store.path.find_last_of('/'));
    int parent=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(parent>=0){fsync(parent);close(parent);}
    store.mainValid=true;
    auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count();
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransPrefs","PREFS_COMMIT entries=%{public}zu bytes=%{public}zu time_us=%{public}lld",values.size(),bytes.size(),(long long)elapsed);return true;
}
bool equalValues(const Values &left,const Values &right){
    if(left.size()!=right.size())return false;auto a=left.begin(),b=right.begin();
    for(;a!=left.end();++a,++b){if(a->first!=b->first||a->second.type!=b->second.type)return false;
        const auto &x=a->second;const auto &y=b->second;
        if(x.type==Type::String){if(x.text!=y.text)return false;}
        else if(x.type==Type::Float){if(memcmp(&x.number,&y.number,sizeof(float)))return false;}
        else if(x.integer!=y.integer)return false;
    }return true;
}
}
std::shared_ptr<Store> Open(const std::string &name){
    std::lock_guard<std::mutex> lock(registryMutex);auto found=registry.find(name);if(found!=registry.end())return found->second;
    auto store=openStore(root,name);if(store)registry[name]=store;return store;
}
bool Read(const std::shared_ptr<Store> &store,const std::string &key,Value &out){
    if(!store)return false;std::lock_guard<std::mutex> lock(store->mutex);auto found=store->values.find(key);
    if(found==store->values.end())return false;out=found->second;return true;
}
Values Snapshot(const std::shared_ptr<Store> &store){if(!store)return {};std::lock_guard<std::mutex> lock(store->mutex);return store->values;}
bool Apply(const std::shared_ptr<Editor> &editor){
    if(!editor||!editor->store)return false;
    std::lock_guard<std::mutex> edits(editor->mutex);auto &store=*editor->store;std::lock_guard<std::mutex> lock(store.mutex);
    Values next=editor->clear?Values{}:store.values;
    for(const auto &key:editor->removed)next.erase(key);for(const auto &item:editor->pending)next[item.first]=item.second;
    // Android also skips a disk generation when the final contents did not change.
    // In particular clear()+put(all existing keys) must not force a full fsync cycle.
    if(store.mainValid&&equalValues(next,store.values)){
        editor->pending.clear();editor->removed.clear();editor->clear=false;return true;
    }
    if(!save(store,next)){OH_LOG_Print(LOG_APP,LOG_ERROR,0,"TransPrefs","PREFS_COMMIT_FAILED errno=%{public}d",errno);return false;}
    store.values.swap(next);editor->pending.clear();editor->removed.clear();editor->clear=false;return true;
}
bool SelfTest(){
    // Isolated from real game namespaces. Reopening reads actual bytes, not the registry cache.
    const char *testRoot="/data/storage/el2/base/files/rea-jprefs-selftest";
    auto store=openStore(testRoot,"contract");if(!store)return false;auto edit=std::make_shared<Editor>();edit->store=store;edit->clear=true;
    Value text;text.text=std::string("\xe4\xb8\xad\xe6\x96\x87\0value",12);edit->pending["text"]=text;
    Value integer;integer.type=Type::Long;integer.integer=-1234567890123LL;edit->pending["long"]=integer;
    Value number;number.type=Type::Float;number.number=0.875f;edit->pending["float"]=number;
    bool okay=Apply(edit);auto fresh=openStore(testRoot,"contract");Value actual;
    okay=okay&&fresh&&Read(fresh,"text",actual)&&actual.text==text.text;
    okay=okay&&Read(fresh,"long",actual)&&actual.integer==integer.integer;
    okay=okay&&Read(fresh,"float",actual)&&actual.number==0.875f;
    struct stat before{},after{};okay=okay&&stat(store->path.c_str(),&before)==0;
    edit->clear=true;edit->pending=store->values;okay=okay&&Apply(edit)&&stat(store->path.c_str(),&after)==0&&before.st_ino==after.st_ino;
    edit->removed.insert("text");okay=okay&&Apply(edit);fresh=openStore(testRoot,"contract");
    okay=okay&&fresh&&!Read(fresh,"text",actual)&&Read(fresh,"long",actual);
    // The backup contains the previous full generation, independent of current-file updates.
    Values backup;bool exists=false;okay=okay&&load(store->path+".bak","contract",backup,exists)&&backup.count("text")==1;
    auto other=openStore(testRoot,"other");okay=okay&&other&&Snapshot(other).empty();
    edit->clear=true;okay=okay&&Apply(edit);fresh=openStore(testRoot,"contract");okay=okay&&fresh&&Snapshot(fresh).empty();
    OH_LOG_Print(LOG_APP,okay?LOG_INFO:LOG_ERROR,0,"TransPrefs","PREFS_DISK_SELFTEST %{public}s (reopen, UTF8/NUL, int64, float, remove, backup, namespace, clear)",okay?"PASS":"FAIL");return okay;
}
}
