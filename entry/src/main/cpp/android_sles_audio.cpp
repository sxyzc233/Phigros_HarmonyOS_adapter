// Android OpenSL ES queue ABI for the original NativeAudio plugin. PCM is
// consumed by a single real OHAudio renderer; game/plugin code stays original.
#include <cstdint>
#include <cstddef>
// This library exposes ANDROID's ABI, never OH OpenSL objects. The installed
// OH OpenSLES_Platform.h typedefs sl_uint32_t to unsigned long (8 bytes on
// this target), while the original Android plugin uses fixed 4-byte integers.
// Select bionic's platform scalar widths before importing the standard vtables.
// Do not change the SDK or pass these foreign structures to OH OpenSL.
#define _OPENSLES_PLATFORM_H_
#define SLAPIENTRY
typedef uint8_t sl_uint8_t;
typedef int8_t sl_int8_t;
typedef uint16_t sl_uint16_t;
typedef int16_t sl_int16_t;
typedef uint32_t sl_uint32_t;
typedef int32_t sl_int32_t;
#include <SLES/OpenSLES.h>
static_assert(sizeof(SLuint32)==4&&sizeof(SLboolean)==4&&sizeof(SLresult)==4,"Android OpenSL scalar ABI");
static_assert(sizeof(SLDataFormat_PCM)==28&&offsetof(SLDataFormat_PCM,samplesPerSec)==8,"Android PCM ABI");
static_assert(sizeof(SLInterfaceID_)==16&&sizeof(SLBufferQueueState)==8,"Android UUID/queue ABI");
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <hilog/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct AndroidQueueVtable;
using AndroidQueue = const AndroidQueueVtable * const *;
using QueueCallback = void (*)(AndroidQueue,void *);
struct AndroidQueueVtable {
    SLresult (*Enqueue)(AndroidQueue,const void *,SLuint32);
    SLresult (*Clear)(AndroidQueue);
    SLresult (*GetState)(AndroidQueue,SLBufferQueueState *);
    SLresult (*RegisterCallback)(AndroidQueue,QueueCallback,void *);
};
// These IDs are private to this dependency and its original E7 consumer. No
// system OpenSL objects receive them (OH's queue has a different vtable ABI).
static const SLInterfaceID_ ids[4]={{1,0,0,0,{0}},{2,0,0,0,{0}}, {3,0,0,0,{0}}, {4,0,0,0,{0}}};
extern "C" {
extern const SLInterfaceID SL_IID_ENGINE=&ids[0];
extern const SLInterfaceID SL_IID_PLAY=&ids[1];
extern const SLInterfaceID SL_IID_VOLUME=&ids[2];
extern const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE=&ids[3];
}
namespace {
struct Object;
template<class T> struct Interface { const T *vtable; Object *owner; };
struct Buffer { const unsigned char *data; size_t size,offset; };
struct Object {
    Interface<SLObjectItf_> object;
    Interface<SLEngineItf_> engine;
    Interface<SLPlayItf_> play;
    Interface<SLVolumeItf_> volume;
    Interface<AndroidQueueVtable> queue;
    std::mutex mutex;
    std::deque<Buffer> buffers;
    QueueCallback callback=nullptr;
    void *context=nullptr;
    int kind=0,channels=2,rate=48000;
    SLuint32 state=SL_OBJECT_STATE_UNREALIZED,playState=SL_PLAYSTATE_STOPPED,index=0;
    uint64_t frames=0;
    SLmillibel millibels=0;
    SLpermille pan=0;
    bool stereoPosition=false,mute=false,dead=false;
    float gain=1;
};
template<class T> Object *Owner(T self){return reinterpret_cast<const Interface<void> *>(self)->owner;}
static SLObjectItf_ objectTable;
static SLEngineItf_ engineTable;
static SLPlayItf_ playTable;
static SLVolumeItf_ volumeTable;
static AndroidQueueVtable queueTable;
static std::once_flag tablesOnce;
static std::mutex listMutex,rendererMutex;
static std::vector<std::shared_ptr<Object>> objects;
static OH_AudioRenderer *renderer=nullptr;
static std::atomic<unsigned long> pcmBlocks{0},pcmNonzero{0};
SLresult Unsupported(){return SL_RESULT_FEATURE_UNSUPPORTED;}
template<class T> void UnsupportedTable(T &table){
    static_assert(sizeof(T)%sizeof(void *)==0,"OpenSL vtable");
    for(size_t i=0;i<sizeof(T)/sizeof(void *);++i)reinterpret_cast<void **>(&table)[i]=(void *)&Unsupported;
}
SLresult Realize(SLObjectItf self,SLboolean){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->state=SL_OBJECT_STATE_REALIZED;return SL_RESULT_SUCCESS;}
SLresult ObjectState(SLObjectItf self,SLuint32 *out){if(!out)return SL_RESULT_PARAMETER_INVALID;*out=Owner(self)->state;return SL_RESULT_SUCCESS;}
SLresult GetInterface(SLObjectItf self,SLInterfaceID id,void *out){
    if(!out)return SL_RESULT_PARAMETER_INVALID;
    auto *o=Owner(self);void *value=nullptr;
    if(id==SL_IID_ENGINE&&o->kind==0)value=&o->engine;
    if(id==SL_IID_PLAY&&o->kind==2)value=&o->play;
    if(id==SL_IID_VOLUME&&o->kind==2)value=&o->volume;
    if(id==SL_IID_ANDROIDSIMPLEBUFFERQUEUE&&o->kind==2)value=&o->queue;
    *static_cast<void **>(out)=value;
    return value?SL_RESULT_SUCCESS:SL_RESULT_FEATURE_UNSUPPORTED;
}
void Destroy(SLObjectItf self){
    auto *o=Owner(self);
    {std::lock_guard<std::mutex> l(o->mutex);o->dead=true;o->buffers.clear();o->callback=nullptr;}
    std::lock_guard<std::mutex> l(listMutex);
    objects.erase(std::remove_if(objects.begin(),objects.end(),[o](const auto &p){return p.get()==o;}),objects.end());
}
SLresult PlayState(SLPlayItf self,SLuint32 state){
    if(state<SL_PLAYSTATE_STOPPED||state>SL_PLAYSTATE_PLAYING)return SL_RESULT_PARAMETER_INVALID;
    auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->playState=state;
    if(state==SL_PLAYSTATE_STOPPED)o->frames=0;
    return SL_RESULT_SUCCESS;
}
SLresult GetPlayState(SLPlayItf self,SLuint32 *out){if(!out)return SL_RESULT_PARAMETER_INVALID;auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);*out=o->playState;return SL_RESULT_SUCCESS;}
SLresult Position(SLPlayItf self,SLmillisecond *out){if(!out)return SL_RESULT_PARAMETER_INVALID;auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);*out=o->frames*1000/o->rate;return SL_RESULT_SUCCESS;}
SLresult Enqueue(AndroidQueue self,const void *data,SLuint32 size){
    auto *o=Owner(self);if(!data||!size||size%(o->channels*2))return SL_RESULT_PARAMETER_INVALID;
    std::lock_guard<std::mutex> l(o->mutex);if(o->dead)return SL_RESULT_PRECONDITIONS_VIOLATED;
    if(o->buffers.size()>=32)return SL_RESULT_BUFFER_INSUFFICIENT;
    o->buffers.push_back({static_cast<const unsigned char *>(data),size,0});return SL_RESULT_SUCCESS;
}
SLresult Clear(AndroidQueue self){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->buffers.clear();o->index=0;return SL_RESULT_SUCCESS;}
SLresult QueueState(AndroidQueue self,SLBufferQueueState *out){if(!out)return SL_RESULT_PARAMETER_INVALID;auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);out->count=o->buffers.size();out->playIndex=o->index;return SL_RESULT_SUCCESS;}
SLresult Callback(AndroidQueue self,QueueCallback cb,void *ctx){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->callback=cb;o->context=ctx;return SL_RESULT_SUCCESS;}
SLresult Volume(SLVolumeItf self,SLmillibel level){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->millibels=level;o->gain=std::pow(10.0f,level/2000.0f);return SL_RESULT_SUCCESS;}
SLresult GetVolume(SLVolumeItf self,SLmillibel *out){if(!out)return SL_RESULT_PARAMETER_INVALID;auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);*out=o->millibels;return SL_RESULT_SUCCESS;}
SLresult MaxVolume(SLVolumeItf,SLmillibel *out){if(!out)return SL_RESULT_PARAMETER_INVALID;*out=0;return SL_RESULT_SUCCESS;}
SLresult Mute(SLVolumeItf self,SLboolean on){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->mute=on;return SL_RESULT_SUCCESS;}
SLresult PanEnable(SLVolumeItf self,SLboolean on){auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->stereoPosition=on;return SL_RESULT_SUCCESS;}
SLresult Pan(SLVolumeItf self,SLpermille pan){if(pan<-1000||pan>1000)return SL_RESULT_PARAMETER_INVALID;auto *o=Owner(self);std::lock_guard<std::mutex> l(o->mutex);o->pan=pan;return SL_RESULT_SUCCESS;}

int32_t Write(OH_AudioRenderer *,void *,void *data,int32_t bytes){
    auto *output=static_cast<int16_t *>(data);std::memset(data,0,bytes);
    std::array<std::shared_ptr<Object>,64> current;size_t count=0;
    {std::lock_guard<std::mutex> l(listMutex);for(auto &o:objects)if(o->kind==2&&count<current.size())current[count++]=o;}
    // Keep accumulators bounded independent of the renderer's chosen callback size.
    for(int start=0;start<bytes/4;start+=256){
        int n=std::min(256,bytes/4-start);int32_t mix[512]{};
        for(size_t i=0;i<count;++i){auto *o=current[i].get();int done=0;
            while(done<n){QueueCallback cb=nullptr;void *ctx=nullptr;bool empty=false;
                {std::lock_guard<std::mutex> l(o->mutex);
                    if(o->dead||o->playState!=SL_PLAYSTATE_PLAYING||o->buffers.empty())break;
                    auto &b=o->buffers.front();int frames=std::min<size_t>(n-done,(b.size-b.offset)/(o->channels*2));
                    auto *samples=reinterpret_cast<const int16_t *>(b.data+b.offset);
                    float gain=o->mute?0:o->gain;
                    float left=gain,right=gain;
                    if(o->stereoPosition){if(o->pan>0)left*=1-o->pan/1000.f;else right*=1+o->pan/1000.f;}
                    for(int j=0;j<frames;++j){mix[(done+j)*2]+=samples[j*o->channels]*left;mix[(done+j)*2+1]+=samples[j*o->channels+o->channels-1]*right;}
                    done+=frames;b.offset+=frames*o->channels*2;o->frames+=frames;
                    if(b.offset==b.size){o->buffers.pop_front();++o->index;cb=o->callback;ctx=o->context;}
                    empty=frames==0;
                }
                // Original doubleBufferingCallback may Enqueue/clear the queue.
                // Never hold the bridge lock while invoking it.
                if(cb)cb(reinterpret_cast<AndroidQueue>(&o->queue),ctx);
                if(empty)break;
            }
        }
        for(int j=0;j<n*2;++j){auto value=std::max(-32768,std::min(32767,mix[j]));output[start*2+j]=value;if(value)++pcmNonzero;}
    }
    auto blocks=++pcmBlocks;
    if(blocks==1||blocks%2000==0)OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransAudio","E7_PCM blocks=%{public}lu nonzero=%{public}lu",blocks,pcmNonzero.load());
    return 0;
}
bool EnsureRenderer(){
    std::lock_guard<std::mutex> l(rendererMutex);if(renderer)return true;
    OH_AudioStreamBuilder *b=nullptr;
    auto result=OH_AudioStreamBuilder_Create(&b,AUDIOSTREAM_TYPE_RENDERER);
    if(result||!b)return false;
    OH_AudioRenderer_Callbacks callbacks{};callbacks.OH_AudioRenderer_OnWriteData=Write;
    OH_AudioStreamBuilder_SetSamplingRate(b,48000);
    OH_AudioStreamBuilder_SetChannelCount(b,2);
    OH_AudioStreamBuilder_SetSampleFormat(b,AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(b,AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetLatencyMode(b,AUDIOSTREAM_LATENCY_MODE_FAST);
    OH_AudioStreamBuilder_SetRendererInfo(b,AUDIOSTREAM_USAGE_GAME);
    OH_AudioStreamBuilder_SetFrameSizeInCallback(b,192);
    OH_AudioStreamBuilder_SetRendererCallback(b,callbacks,nullptr);
    result=OH_AudioStreamBuilder_GenerateRenderer(b,&renderer);
    OH_AudioStreamBuilder_Destroy(b);
    if(result||!renderer){renderer=nullptr;return false;}
    result=OH_AudioRenderer_Start(renderer);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"TransAudio","E7_RENDERER start=%{public}d",result);
    if(result){OH_AudioRenderer_Release(renderer);renderer=nullptr;return false;}
    return true;
}
std::shared_ptr<Object> Make(int kind){
    auto o=std::make_shared<Object>();o->kind=kind;
    o->object={&objectTable,o.get()};o->engine={&engineTable,o.get()};o->play={&playTable,o.get()};
    o->volume={&volumeTable,o.get()};o->queue={&queueTable,o.get()};
    std::lock_guard<std::mutex> l(listMutex);objects.push_back(o);return o;
}
SLresult CreateMix(SLEngineItf,SLObjectItf *out,SLuint32,const SLInterfaceID *,const SLboolean *){
    if(!out)return SL_RESULT_PARAMETER_INVALID;auto o=Make(1);*out=reinterpret_cast<SLObjectItf>(&o->object);return SL_RESULT_SUCCESS;
}
SLresult CreatePlayer(SLEngineItf,SLObjectItf *out,SLDataSource *source,SLDataSink *,SLuint32 n,const SLInterfaceID *requested,const SLboolean *required){
    if(!out||!source||!source->pFormat)return SL_RESULT_PARAMETER_INVALID;
    *out=nullptr;auto *pcm=static_cast<SLDataFormat_PCM *>(source->pFormat);
    if(pcm->formatType!=SL_DATAFORMAT_PCM||pcm->samplesPerSec!=SL_SAMPLINGRATE_48||pcm->bitsPerSample!=16||pcm->containerSize!=16||pcm->endianness!=SL_BYTEORDER_LITTLEENDIAN||(pcm->numChannels!=1&&pcm->numChannels!=2))return SL_RESULT_CONTENT_UNSUPPORTED;
    for(unsigned i=0;i<n;++i)if(required&&required[i]&&requested[i]!=SL_IID_PLAY&&requested[i]!=SL_IID_VOLUME&&requested[i]!=SL_IID_ANDROIDSIMPLEBUFFERQUEUE)return SL_RESULT_FEATURE_UNSUPPORTED;
    if(!EnsureRenderer())return SL_RESULT_RESOURCE_ERROR;
    auto o=Make(2);o->channels=pcm->numChannels;*out=reinterpret_cast<SLObjectItf>(&o->object);return SL_RESULT_SUCCESS;
}
void Tables(){
    UnsupportedTable(objectTable);UnsupportedTable(engineTable);UnsupportedTable(playTable);UnsupportedTable(volumeTable);
    objectTable.Realize=Realize;objectTable.GetState=ObjectState;objectTable.GetInterface=GetInterface;objectTable.Destroy=Destroy;
    engineTable.CreateOutputMix=CreateMix;engineTable.CreateAudioPlayer=CreatePlayer;
    playTable.SetPlayState=PlayState;playTable.GetPlayState=GetPlayState;playTable.GetPosition=Position;
    volumeTable.SetVolumeLevel=Volume;volumeTable.GetVolumeLevel=GetVolume;volumeTable.GetMaxVolumeLevel=MaxVolume;
    volumeTable.SetMute=Mute;volumeTable.EnableStereoPosition=PanEnable;volumeTable.SetStereoPosition=Pan;
    queueTable={Enqueue,Clear,QueueState,Callback};
}
}
extern "C" SLresult slCreateEngine(SLObjectItf *out,SLuint32,const SLEngineOption *,SLuint32,const SLInterfaceID *,const SLboolean *){
    if(!out)return SL_RESULT_PARAMETER_INVALID;std::call_once(tablesOnce,Tables);
    auto o=Make(0);*out=reinterpret_cast<SLObjectItf>(&o->object);return SL_RESULT_SUCCESS;
}
