// Replaces only FMODAudioDevice.java's AudioTrack worker. fmodGetInfo and
// fmodProcess remain the verified original libunity JNI implementations.
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <hilog/log.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" void *ReaFrameworkEnv();
extern "C" void *ReaCreateDirectAudioBuffer(void *,size_t);
namespace {
using GetInfo=int (*)(void *,void *,int);
using Process=int (*)(void *,void *,void *);
struct Device {
    std::atomic<bool> active{true},running{false};
    GetInfo info;Process process;
    void *object,*bufferObject=nullptr;
    std::vector<unsigned char> pcm;
    size_t offset=0;
    OH_AudioRenderer *renderer=nullptr;
    unsigned long blocks=0,nonzero=0,errors=0;
};
static std::mutex deviceMutex;
static std::shared_ptr<Device> device;
int32_t Write(OH_AudioRenderer *,void *raw,void *output,int32_t bytes){
    auto *d=static_cast<Device *>(raw);auto *p=static_cast<unsigned char *>(output);
    memset(output,0,bytes);if(!d->active)return 0;
    while(bytes>0){
        if(d->offset==d->pcm.size()){
            if(d->info(ReaFrameworkEnv(),d->object,3)!=1)break;
            int result=d->process(ReaFrameworkEnv(),d->object,d->bufferObject);
            if(result<0){++d->errors;break;}
            d->offset=0;++d->blocks;
            for(auto v:d->pcm)if(v){++d->nonzero;break;}
            if(d->blocks==1||d->blocks%2000==0)OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransAudio","FMOD_PCM blocks=%{public}lu nonzeroBlocks=%{public}lu errors=%{public}lu",d->blocks,d->nonzero,d->errors);
        }
        size_t n=std::min<size_t>(bytes,d->pcm.size()-d->offset);
        memcpy(p,d->pcm.data()+d->offset,n);d->offset+=n;p+=n;bytes-=n;
    }
    return 0;
}
void Worker(std::shared_ptr<Device> d){
    pthread_setname_np(pthread_self(),"ReaFMODAudio");
    int rate=0,channels=0,frames=0;
    for(int i=0;i<500&&d->active;++i){
        rate=d->info(ReaFrameworkEnv(),d->object,0);
        channels=d->info(ReaFrameworkEnv(),d->object,4);
        frames=d->info(ReaFrameworkEnv(),d->object,1);
        if(rate>=8000&&rate<=192000&&channels>=1&&channels<=2&&frames>0&&frames<=8192)break;
        usleep(10000);
    }
    if(!d->active||rate<8000||rate>192000||channels<1||channels>2||frames<=0||frames>8192){
        d->running=false;OH_LOG_Print(LOG_APP,LOG_ERROR,0,"TransAudio","FMOD_INVALID_CONFIG rate=%{public}d channels=%{public}d frames=%{public}d",rate,channels,frames);return;
    }
    d->pcm.resize(frames*channels*2);d->offset=d->pcm.size();
    d->bufferObject=ReaCreateDirectAudioBuffer(d->pcm.data(),d->pcm.size());
    OH_AudioStreamBuilder *b=nullptr;auto result=OH_AudioStreamBuilder_Create(&b,AUDIOSTREAM_TYPE_RENDERER);
    if(!result&&b){
        OH_AudioRenderer_Callbacks cb{};cb.OH_AudioRenderer_OnWriteData=Write;
        OH_AudioStreamBuilder_SetSamplingRate(b,rate);OH_AudioStreamBuilder_SetChannelCount(b,channels);
        OH_AudioStreamBuilder_SetSampleFormat(b,AUDIOSTREAM_SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetEncodingType(b,AUDIOSTREAM_ENCODING_TYPE_RAW);
        OH_AudioStreamBuilder_SetLatencyMode(b,AUDIOSTREAM_LATENCY_MODE_FAST);
        OH_AudioStreamBuilder_SetRendererInfo(b,AUDIOSTREAM_USAGE_GAME);
        OH_AudioStreamBuilder_SetFrameSizeInCallback(b,192);
        OH_AudioStreamBuilder_SetRendererCallback(b,cb,d.get());
        result=OH_AudioStreamBuilder_GenerateRenderer(b,&d->renderer);OH_AudioStreamBuilder_Destroy(b);
        if(!result&&d->renderer)result=OH_AudioRenderer_Start(d->renderer);
    }
    OH_LOG_Print(LOG_APP,result?LOG_ERROR:LOG_INFO,0,"TransAudio","FMOD_RENDERER result=%{public}d rate=%{public}d channels=%{public}d blockFrames=%{public}d",result,rate,channels,frames);
    if(!result&&d->renderer)while(d->active)usleep(10000);
    if(d->renderer){OH_AudioRenderer_Stop(d->renderer);OH_AudioRenderer_Release(d->renderer);d->renderer=nullptr;}
    d->running=false;
}
}
extern "C" void ReaFmodStart(uintptr_t unityBase,void *object){
    std::lock_guard<std::mutex> lock(deviceMutex);
    if(device&&device->active&&device->object==object)return;
    if(device)device->active=false;
    if(!unityBase)return;
    auto d=std::make_shared<Device>();d->object=object;
    // Static JNI table at libunity+1354950/1354968, audited against the original
    // APK libunity SHA256 23ad1f0b...671e1f. Not guessed managed/game methods.
    d->info=reinterpret_cast<GetInfo>(unityBase+0xfe8414);
    d->process=reinterpret_cast<Process>(unityBase+0xfe84dc);
    d->running=true;device=d;
    std::thread(Worker,d).detach();
}
extern "C" void ReaFmodStop(){std::lock_guard<std::mutex> lock(deviceMutex);if(device)device->active=false;}
extern "C" bool ReaFmodRunning(){std::lock_guard<std::mutex> lock(deviceMutex);return device&&device->running&&device->active;}
