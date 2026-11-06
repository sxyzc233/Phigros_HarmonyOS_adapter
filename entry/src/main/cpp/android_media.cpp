// Android Media NDK ABI backed by Harmony's real demuxer/decoders.
// Codec callbacks only enqueue owned buffers. Never run Unity or block its UI
// thread inside a Harmony callback, and never report fake media preparation/EOS.
#include <multimedia/player_framework/native_avsource.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <hilog/log.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <memory>
#include <cmath>
#include <atomic>

#define MEDIA_LOG(...) OH_LOG_Print(LOG_APP,LOG_INFO,0,"ReaMediaShim",__VA_ARGS__)
using media_status_t=int32_t;
constexpr int OK=0, ERROR=-10000, INVALID=-10001, UNSUPPORTED=-10002;
struct AMediaFormat { OH_AVFormat *value=nullptr; explicit AMediaFormat(OH_AVFormat *v):value(v){} ~AMediaFormat(){if(value)OH_AVFormat_Destroy(value);} };
static const char *Key(const char *k){
    if(!k)return "";
    if(!strcmp(k,"mime"))return OH_MD_KEY_CODEC_MIME;
    if(!strcmp(k,"width"))return OH_MD_KEY_WIDTH;
    if(!strcmp(k,"height"))return OH_MD_KEY_HEIGHT;
    if(!strcmp(k,"frame-rate"))return OH_MD_KEY_FRAME_RATE;
    if(!strcmp(k,"sample-rate"))return OH_MD_KEY_AUD_SAMPLE_RATE;
    if(!strcmp(k,"channel-count"))return OH_MD_KEY_AUD_CHANNEL_COUNT;
    if(!strcmp(k,"language"))return OH_MD_KEY_LANGUAGE;
    if(!strcmp(k,"max-input-size"))return OH_MD_KEY_MAX_INPUT_SIZE;
    if(!strcmp(k,"color-range"))return OH_MD_KEY_RANGE_FLAG;
    if(!strcmp(k,"durationUs"))return OH_MD_KEY_DURATION;
    if(!strcmp(k,"color-format"))return OH_MD_KEY_PIXEL_FORMAT;
    if(!strcmp(k,"stride"))return OH_MD_KEY_VIDEO_STRIDE;
    if(!strcmp(k,"slice-height"))return OH_MD_KEY_VIDEO_SLICE_HEIGHT;
    if(!strcmp(k,"rotation-degrees"))return OH_MD_KEY_ROTATION;
    if(!strcmp(k,"crop-left"))return OH_MD_KEY_VIDEO_CROP_LEFT;
    if(!strcmp(k,"crop-right"))return OH_MD_KEY_VIDEO_CROP_RIGHT;
    if(!strcmp(k,"crop-top"))return OH_MD_KEY_VIDEO_CROP_TOP;
    if(!strcmp(k,"crop-bottom"))return OH_MD_KEY_VIDEO_CROP_BOTTOM;
    if(!strcmp(k,"csd-0"))return OH_MD_KEY_CODEC_CONFIG;
    return k;
}
static int Status(OH_AVErrCode e){return e==AV_ERR_OK?OK:ERROR;}
extern "C" {
const char *AMEDIAFORMAT_KEY_MIME="mime", *AMEDIAFORMAT_KEY_WIDTH="width", *AMEDIAFORMAT_KEY_HEIGHT="height";
const char *AMEDIAFORMAT_KEY_STRIDE="stride", *AMEDIAFORMAT_KEY_SLICE_HEIGHT="slice-height", *AMEDIAFORMAT_KEY_COLOR_FORMAT="color-format";
const char *AMEDIAFORMAT_KEY_COLOR_RANGE="color-range", *AMEDIAFORMAT_KEY_COLOR_STANDARD="color-standard", *AMEDIAFORMAT_KEY_DURATION="durationUs";
const char *AMEDIAFORMAT_KEY_FRAME_RATE="frame-rate", *AMEDIAFORMAT_KEY_SAMPLE_RATE="sample-rate", *AMEDIAFORMAT_KEY_CHANNEL_COUNT="channel-count";
const char *AMEDIAFORMAT_KEY_LANGUAGE="language", *AMEDIAFORMAT_KEY_ROTATION="rotation-degrees", *AMEDIAFORMAT_KEY_ENCODER_DELAY="encoder-delay";
AMediaFormat *AMediaFormat_new(){return new AMediaFormat(OH_AVFormat_Create());}
void AMediaFormat_delete(AMediaFormat *f){delete f;}
bool AMediaFormat_getInt32(AMediaFormat *f,const char *k,int32_t *out){
    if(!f||!f->value||!out)return false;int32_t v=0;
    if(!OH_AVFormat_GetIntValue(f->value,Key(k),&v)){
        double number=0;
        if(strcmp(k,"frame-rate")||!OH_AVFormat_GetDoubleValue(f->value,Key(k),&number)||!std::isfinite(number)||number<0||number>1000)return false;
        v=std::lround(number);
    }
    if(!strcmp(k,"color-format")){
        if(v==AV_PIXEL_FORMAT_YUVI420)v=19;else if(v==AV_PIXEL_FORMAT_NV12)v=21;
        else if(v==AV_PIXEL_FORMAT_NV21)v=0x7fa30c00;else return false;
    }
    static std::atomic<unsigned> reads{0};if(reads++<80)MEDIA_LOG("MEDIA_FORMAT_INT key=%{public}s value=%{public}d",k,v);
    *out=v;return true;
}
bool AMediaFormat_getInt64(AMediaFormat *f,const char *k,int64_t *out){return f&&out&&OH_AVFormat_GetLongValue(f->value,Key(k),out);}
bool AMediaFormat_getFloat(AMediaFormat *f,const char *k,float *out){
    if(!f||!out)return false;if(OH_AVFormat_GetFloatValue(f->value,Key(k),out))return true;
    double d=0;if(OH_AVFormat_GetDoubleValue(f->value,Key(k),&d)){*out=d;return true;}
    int32_t i=0;if(OH_AVFormat_GetIntValue(f->value,Key(k),&i)){*out=i;return true;}return false;
}
bool AMediaFormat_getString(AMediaFormat *f,const char *k,const char **out){bool result=f&&out&&OH_AVFormat_GetStringValue(f->value,Key(k),out);MEDIA_LOG("MEDIA_FORMAT_STRING key=%{public}s found=%{public}d",k?k:"null",result);return result;}
bool AMediaFormat_getBuffer(AMediaFormat *f,const char *k,void **out,size_t *size){return f&&out&&size&&OH_AVFormat_GetBuffer(f->value,Key(k),(uint8_t **)out,size);}
void AMediaFormat_setInt32(AMediaFormat *f,const char *k,int32_t v){
    if(!f)return;if(!strcmp(k,"color-format")){if(v==19)v=AV_PIXEL_FORMAT_YUVI420;else if(v==21)v=AV_PIXEL_FORMAT_NV12;else return;}
    OH_AVFormat_SetIntValue(f->value,Key(k),v);
}
void AMediaFormat_setInt64(AMediaFormat *f,const char *k,int64_t v){if(f)OH_AVFormat_SetLongValue(f->value,Key(k),v);}
void AMediaFormat_setFloat(AMediaFormat *f,const char *k,float v){if(f)OH_AVFormat_SetFloatValue(f->value,Key(k),v);}
void AMediaFormat_setString(AMediaFormat *f,const char *k,const char *v){if(f&&v)OH_AVFormat_SetStringValue(f->value,Key(k),v);}
void AMediaFormat_setBuffer(AMediaFormat *f,const char *k,const void *v,size_t n){if(f&&v)OH_AVFormat_SetBuffer(f->value,Key(k),(const uint8_t *)v,n);}
}

using ReadAt=ssize_t (*)(void *,int64_t,void *,size_t);
struct AMediaDataSource { ReadAt readAt=nullptr;int64_t (*getSize)(void *)=nullptr;void (*close)(void *)=nullptr;void *user=nullptr; };
struct SourceState {AMediaDataSource callbacks;OH_AVDataSourceExt data{};std::mutex mutex;int64_t position=0;};
struct Sample {
    uint32_t track=0;OH_AVBuffer *buffer=nullptr;OH_AVCodecBufferAttr attr{};bool ready=false,eos=false;
    ~Sample(){if(buffer)OH_AVBuffer_Destroy(buffer);}
};
struct AMediaExtractor {
    OH_AVSource *source=nullptr;OH_AVDemuxer *demuxer=nullptr;int fd=-1;int32_t tracks=0;int64_t duration=-1;
    std::unique_ptr<SourceState> custom;std::map<uint32_t,std::unique_ptr<Sample>> selected;
    ~AMediaExtractor(){Reset();}
    void Reset(){selected.clear();if(demuxer)OH_AVDemuxer_Destroy(demuxer);demuxer=nullptr;
        if(source)OH_AVSource_Destroy(source);source=nullptr;if(fd>=0)close(fd);fd=-1;custom.reset();tracks=0;}
    int Initialize(){
        if(!source)return ERROR;auto *format=OH_AVSource_GetSourceFormat(source);
        bool okay=format&&OH_AVFormat_GetIntValue(format,OH_MD_KEY_TRACK_COUNT,&tracks);
        if(format){OH_AVFormat_GetLongValue(format,OH_MD_KEY_DURATION,&duration);OH_AVFormat_Destroy(format);}if(!okay||tracks<1||tracks>128)return INVALID;
        demuxer=OH_AVDemuxer_CreateWithSource(source);
        MEDIA_LOG("MEDIA_SOURCE tracks=%{public}d ready=%{public}d",tracks,demuxer?1:0);return demuxer?OK:ERROR;
    }
    bool Read(Sample &s){
        if(s.eos)return false;if(s.ready)return true;
        if(!s.buffer)s.buffer=OH_AVBuffer_Create(8*1024*1024);
        if(!s.buffer){s.eos=true;return false;}
        auto result=OH_AVDemuxer_ReadSampleBuffer(demuxer,s.track,s.buffer);
        if(result!=AV_ERR_OK||OH_AVBuffer_GetBufferAttr(s.buffer,&s.attr)!=AV_ERR_OK){
            MEDIA_LOG("MEDIA_SAMPLE_ERROR track=%{public}u result=%{public}d",s.track,result);s.eos=true;return false;}
        if(s.attr.flags&AVCODEC_BUFFER_FLAGS_EOS){s.eos=true;return false;}
        s.ready=true;return true;
    }
    Sample *Current(){Sample *best=nullptr;for(auto &item:selected){auto &s=*item.second;
        if(Read(s)&&(!best||s.attr.pts<best->attr.pts))best=&s;}return best;}
};
static int32_t CustomRead(OH_AVBuffer *buffer,int32_t requested,int64_t pos,void *user){
    auto *s=static_cast<SourceState *>(user);if(!s||!s->callbacks.readAt||!buffer||requested<=0)return -1;
    std::lock_guard<std::mutex> lock(s->mutex);if(pos<0)pos=s->position;
    int32_t capacity=OH_AVBuffer_GetCapacity(buffer);auto *data=OH_AVBuffer_GetAddr(buffer);
    if(!data||capacity<=0||pos<0)return -1;if(pos>=s->data.size)return -1;
    size_t count=std::min<int64_t>(std::min(requested,capacity),s->data.size-pos);
    auto n=s->callbacks.readAt(s->callbacks.user,pos,data,count);
    if(n<=0||size_t(n)>count)return -1;s->position=pos+n;return n;
}
extern "C" {
AMediaDataSource *AMediaDataSource_new(){return new AMediaDataSource;}
void AMediaDataSource_delete(AMediaDataSource *s){delete s;}
void AMediaDataSource_setReadAt(AMediaDataSource *s,ReadAt f){if(s)s->readAt=f;}
void AMediaDataSource_setGetSize(AMediaDataSource *s,int64_t (*f)(void *)){if(s)s->getSize=f;}
void AMediaDataSource_setClose(AMediaDataSource *s,void (*f)(void *)){if(s)s->close=f;}
void AMediaDataSource_setUserdata(AMediaDataSource *s,void *user){if(s)s->user=user;}
AMediaExtractor *AMediaExtractor_new(){return new AMediaExtractor;}
void AMediaExtractor_delete(AMediaExtractor *e){delete e;}
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *e,int fd,int64_t offset,int64_t length){
    if(!e||fd<0||offset<0||length<=0)return INVALID;e->Reset();e->fd=dup(fd);if(e->fd<0)return ERROR;
    e->source=OH_AVSource_CreateWithFD(e->fd,offset,length);return e->Initialize();
}
media_status_t AMediaExtractor_setDataSource(AMediaExtractor *e,const char *path){
    if(!e||!path)return INVALID;
    if(!strncmp(path,"http://",7)||!strncmp(path,"https://",8)){
        e->Reset();std::string uri(path);e->source=OH_AVSource_CreateWithURI(&uri[0]);return e->Initialize();}
    if(!strncmp(path,"file://",7))path+=7;int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return ERROR;
    struct stat st{};int result=fstat(fd,&st)==0?AMediaExtractor_setDataSourceFd(e,fd,0,st.st_size):ERROR;close(fd);return result;
}
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *e,AMediaDataSource *s){
    if(!e||!s||!s->readAt||!s->getSize)return INVALID;e->Reset();e->custom.reset(new SourceState);
    auto &state=*e->custom;state.callbacks=*s;state.data.size=s->getSize(s->user);state.data.readAt=CustomRead;
    if(state.data.size<=0)return INVALID;e->source=OH_AVSource_CreateWithDataSourceExt(&state.data,&state);return e->Initialize();
}
size_t AMediaExtractor_getTrackCount(AMediaExtractor *e){return e?e->tracks:0;}
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *e,size_t index){
    if(!e||!e->source||index>=size_t(e->tracks))return nullptr;
    auto *f=OH_AVSource_GetTrackFormat(e->source,index);
    int64_t duration=0;if(f&&!OH_AVFormat_GetLongValue(f,OH_MD_KEY_DURATION,&duration)&&e->duration>=0)OH_AVFormat_SetLongValue(f,OH_MD_KEY_DURATION,e->duration);
    double fps=0;int32_t fpsInt=0;int64_t actualDuration=-1;
    if(f){if(!OH_AVFormat_GetDoubleValue(f,OH_MD_KEY_FRAME_RATE,&fps)&&OH_AVFormat_GetIntValue(f,OH_MD_KEY_FRAME_RATE,&fpsInt))fps=fpsInt;
        OH_AVFormat_GetLongValue(f,OH_MD_KEY_DURATION,&actualDuration);}
    MEDIA_LOG("MEDIA_TRACK_INFO index=%{public}llu duration_us=%{public}lld fps=%{public}.3f",(unsigned long long)index,(long long)actualDuration,fps);
    return f?new AMediaFormat(f):nullptr;
}
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *e,size_t index){
    if(!e||!e->demuxer||index>=size_t(e->tracks))return INVALID;if(e->selected.count(index))return OK;
    auto result=OH_AVDemuxer_SelectTrackByID(e->demuxer,index);if(result!=AV_ERR_OK)return ERROR;
    std::unique_ptr<Sample> s(new Sample);s->track=index;e->selected[index]=std::move(s);return OK;
}
media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *e,size_t index){
    if(!e||!e->demuxer)return INVALID;auto r=OH_AVDemuxer_UnselectTrackByID(e->demuxer,index);e->selected.erase(index);return Status(r);
}
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *e,uint8_t *buffer,size_t capacity){
    auto *s=e?e->Current():nullptr;if(!s)return -1;
    if(s->attr.size<0||s->attr.offset<0||size_t(s->attr.size)>capacity||!buffer)return -1;
    auto *data=OH_AVBuffer_GetAddr(s->buffer);int32_t n=OH_AVBuffer_GetCapacity(s->buffer);
    if(!data||s->attr.offset>n||s->attr.size>n-s->attr.offset)return -1;
    memcpy(buffer,data+s->attr.offset,s->attr.size);return s->attr.size;
}
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *e){auto *s=e?e->Current():nullptr;return s?int(s->track):-1;}
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *e){auto *s=e?e->Current():nullptr;return s?s->attr.pts:-1;}
uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *e){auto *s=e?e->Current():nullptr;return s&&s->attr.flags&AVCODEC_BUFFER_FLAGS_SYNC_FRAME?1:0;}
bool AMediaExtractor_advance(AMediaExtractor *e){auto *s=e?e->Current():nullptr;if(!s)return false;s->ready=false;return e->Current()!=nullptr;}
media_status_t AMediaExtractor_seekTo(AMediaExtractor *e,int64_t timeUs,int mode){
    if(!e||!e->demuxer||timeUs<0||mode<0||mode>2)return INVALID;
    OH_AVSeekMode modes[]={SEEK_MODE_PREVIOUS_SYNC,SEEK_MODE_NEXT_SYNC,SEEK_MODE_CLOSEST_SYNC};
    auto r=OH_AVDemuxer_SeekToTime(e->demuxer,timeUs/1000,modes[mode]);
    if(r==AV_ERR_OK)for(auto &s:e->selected){s.second->ready=false;s.second->eos=false;}return Status(r);
}
}

// Android's layout is offset,size,time,flags (different from OH_AVCodecBufferAttr).
struct AMediaCodecBufferInfo {int32_t offset,size;int64_t presentationTimeUs;uint32_t flags;};
static_assert(offsetof(AMediaCodecBufferInfo,presentationTimeUs)==8,"Android ABI");
struct AMediaCodec {
    OH_AVCodec *codec=nullptr;bool video=false;std::mutex mutex;std::condition_variable changed;
    std::deque<uint32_t> inputs,outputs;std::map<uint32_t,OH_AVBuffer *> inputBuffers,outputBuffers;
    bool active=false,stopping=false,formatChanged=false,reportedFormat=false;int error=0;uint64_t decoded=0,released=0;int64_t pendingOutput=-1;
    OH_AVFormat *description=nullptr;
};
static uint32_t ToOHFlags(uint32_t f){return ((f&1)?AVCODEC_BUFFER_FLAGS_SYNC_FRAME:0)|((f&2)?AVCODEC_BUFFER_FLAGS_CODEC_DATA:0)|((f&4)?AVCODEC_BUFFER_FLAGS_EOS:0);}
static uint32_t ToAndroidFlags(uint32_t f){return ((f&AVCODEC_BUFFER_FLAGS_SYNC_FRAME)?1:0)|((f&AVCODEC_BUFFER_FLAGS_CODEC_DATA)?2:0)|((f&AVCODEC_BUFFER_FLAGS_EOS)?4:0);}
static void CodecError(OH_AVCodec *,int32_t error,void *user){auto *c=static_cast<AMediaCodec *>(user);{std::lock_guard<std::mutex> lock(c->mutex);c->error=error?error:ERROR;}c->changed.notify_all();MEDIA_LOG("MEDIA_CODEC_ERROR result=%{public}d",error);}
static void CodecFormat(OH_AVCodec *,OH_AVFormat *format,void *user){auto *c=static_cast<AMediaCodec *>(user);{std::lock_guard<std::mutex> lock(c->mutex);
    if(c->description)OH_AVFormat_Destroy(c->description);c->description=OH_AVFormat_Create();OH_AVFormat_Copy(c->description,format);c->formatChanged=true;}c->changed.notify_all();}
static void CodecInput(OH_AVCodec *,uint32_t index,OH_AVBuffer *buffer,void *user){auto *c=static_cast<AMediaCodec *>(user);{std::lock_guard<std::mutex> lock(c->mutex);if(c->stopping)return;c->inputBuffers[index]=buffer;c->inputs.push_back(index);}c->changed.notify_all();}
static void CodecOutput(OH_AVCodec *,uint32_t index,OH_AVBuffer *buffer,void *user){auto *c=static_cast<AMediaCodec *>(user);{std::lock_guard<std::mutex> lock(c->mutex);if(c->stopping)return;c->outputBuffers[index]=buffer;c->outputs.push_back(index);++c->decoded;}c->changed.notify_all();}
static bool WaitBuffer(AMediaCodec *c,std::unique_lock<std::mutex> &lock,bool input,int64_t timeout){
    auto ready=[&](){return c->stopping||c->error||!c->active||(!input&&c->formatChanged)||!(input?c->inputs:c->outputs).empty();};
    if(ready())return true;if(!timeout)return false;
    if(timeout<0)c->changed.wait(lock,ready);else c->changed.wait_for(lock,std::chrono::microseconds(timeout),ready);return ready();
}
extern "C" {
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime){
    if(!mime)return nullptr;std::unique_ptr<AMediaCodec> c(new AMediaCodec);c->video=!strncmp(mime,"video/",6);
    c->codec=c->video?OH_VideoDecoder_CreateByMime(mime):OH_AudioCodec_CreateByMime(mime,false);
    if(!c->codec){MEDIA_LOG("MEDIA_CODEC_CREATE_FAILED mime=%{public}s",mime);return nullptr;}
    MEDIA_LOG("MEDIA_CODEC_CREATED mime=%{public}s",mime);return c.release();
}
AMediaCodec *AMediaCodec_createEncoderByType(const char *){return nullptr;}
media_status_t AMediaCodec_configure(AMediaCodec *c,const AMediaFormat *format,void *surface,void *crypto,uint32_t flags){
    if(!c||!format||!format->value)return INVALID;if(crypto||flags)return UNSUPPORTED;
    OH_AVFormat *f=OH_AVFormat_Create();OH_AVFormat_Copy(f,format->value);
    OH_AVFormat_SetIntValue(f,OH_MD_KEY_ENABLE_SYNC_MODE,1);
    if(!c->video)OH_AVFormat_SetIntValue(f,OH_MD_KEY_AUDIO_SAMPLE_FORMAT,SAMPLE_S16LE);
    auto r=c->video?OH_VideoDecoder_Configure(c->codec,f):OH_AudioCodec_Configure(c->codec,f);OH_AVFormat_Destroy(f);
    if(r==AV_ERR_OK&&c->video&&surface)r=OH_VideoDecoder_SetSurface(c->codec,static_cast<OHNativeWindow *>(surface));
    if(r==AV_ERR_OK)r=c->video?OH_VideoDecoder_Prepare(c->codec):OH_AudioCodec_Prepare(c->codec);
    MEDIA_LOG("MEDIA_CODEC_CONFIGURE video=%{public}d surface=%{public}d result=%{public}d",c->video,surface?1:0,r);return Status(r);
}
media_status_t AMediaCodec_start(AMediaCodec *c){
    if(!c)return INVALID;{std::lock_guard<std::mutex> lock(c->mutex);c->stopping=false;c->active=true;c->error=0;}
    auto r=c->video?OH_VideoDecoder_Start(c->codec):OH_AudioCodec_Start(c->codec);
    if(r!=AV_ERR_OK){std::lock_guard<std::mutex> lock(c->mutex);c->active=false;c->changed.notify_all();}return Status(r);
}
media_status_t AMediaCodec_stop(AMediaCodec *c){
    if(!c)return INVALID;{std::lock_guard<std::mutex> lock(c->mutex);c->stopping=true;c->active=false;}c->changed.notify_all();
    auto r=c->video?OH_VideoDecoder_Stop(c->codec):OH_AudioCodec_Stop(c->codec);
    {std::lock_guard<std::mutex> lock(c->mutex);c->inputs.clear();c->outputs.clear();c->inputBuffers.clear();c->outputBuffers.clear();}return Status(r);
}
media_status_t AMediaCodec_delete(AMediaCodec *c){
    if(!c)return INVALID;{std::lock_guard<std::mutex> lock(c->mutex);c->stopping=true;c->active=false;}c->changed.notify_all();
    // Destroy waits for real framework callbacks to finish before freeing userdata.
    auto r=c->video?OH_VideoDecoder_Destroy(c->codec):OH_AudioCodec_Destroy(c->codec);
    if(c->description)OH_AVFormat_Destroy(c->description);MEDIA_LOG("MEDIA_CODEC_DELETE frames=%{public}llu",(unsigned long long)c->decoded);delete c;return Status(r);
}
media_status_t AMediaCodec_flush(AMediaCodec *c){
    if(!c)return INVALID;{std::lock_guard<std::mutex> lock(c->mutex);c->stopping=true;c->active=false;}c->changed.notify_all();
    auto r=c->video?OH_VideoDecoder_Flush(c->codec):OH_AudioCodec_Flush(c->codec);
    {std::lock_guard<std::mutex> lock(c->mutex);c->inputs.clear();c->outputs.clear();c->inputBuffers.clear();c->outputBuffers.clear();c->formatChanged=false;c->pendingOutput=-1;c->reportedFormat=false;}
    if(r!=AV_ERR_OK)return ERROR;return AMediaCodec_start(c);
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c,int64_t timeout){
    if(!c)return ERROR;uint32_t index=0;
    auto r=c->video?OH_VideoDecoder_QueryInputBuffer(c->codec,&index,timeout):OH_AudioCodec_QueryInputBuffer(c->codec,&index,timeout);
    if(r==AV_ERR_TRY_AGAIN_LATER)return -1;if(r!=AV_ERR_OK){MEDIA_LOG("MEDIA_INPUT_QUERY_ERROR video=%{public}d result=%{public}d",c->video,r);return ERROR;}
    return index;
}
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c,size_t index,size_t *size){
    if(size)*size=0;if(!c)return nullptr;
    auto *buffer=c->video?OH_VideoDecoder_GetInputBuffer(c->codec,index):OH_AudioCodec_GetInputBuffer(c->codec,index);
    if(!buffer)return nullptr;{std::lock_guard<std::mutex> lock(c->mutex);c->inputBuffers[index]=buffer;}
    if(size)*size=std::max(0,OH_AVBuffer_GetCapacity(buffer));return OH_AVBuffer_GetAddr(buffer);
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c,size_t index,off_t offset,size_t size,uint64_t time,uint32_t flags){
    if(!c||offset<0||size>INT32_MAX)return INVALID;OH_AVBuffer *b=nullptr;
    {std::lock_guard<std::mutex> lock(c->mutex);auto it=c->inputBuffers.find(index);if(it==c->inputBuffers.end())return INVALID;b=it->second;
     int32_t cap=OH_AVBuffer_GetCapacity(b);if(offset>cap||size>size_t(cap-offset))return INVALID;}
    OH_AVCodecBufferAttr attr{int64_t(time),int32_t(size),int32_t(offset),ToOHFlags(flags)};
    if(OH_AVBuffer_SetBufferAttr(b,&attr)!=AV_ERR_OK)return ERROR;
    return Status(c->video?OH_VideoDecoder_PushInputBuffer(c->codec,index):OH_AudioCodec_PushInputBuffer(c->codec,index));
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c,AMediaCodecBufferInfo *info,int64_t timeout){
    if(!c||!info)return ERROR;uint32_t index=0;
    if(c->pendingOutput>=0){index=c->pendingOutput;c->pendingOutput=-1;}
    else {
        auto r=c->video?OH_VideoDecoder_QueryOutputBuffer(c->codec,&index,timeout):OH_AudioCodec_QueryOutputBuffer(c->codec,&index,timeout);
        if(r==AV_ERR_STREAM_CHANGED){c->reportedFormat=true;return -2;}
        if(r==AV_ERR_TRY_AGAIN_LATER)return -1;
        if(r!=AV_ERR_OK){MEDIA_LOG("MEDIA_OUTPUT_QUERY_ERROR video=%{public}d result=%{public}d",c->video,r);return ERROR;}
        // Android always announces the first real decoded output format. Some
        // OH audio codecs omit STREAM_CHANGED for their initial configuration.
        if(!c->reportedFormat){
            auto *f=c->video?OH_VideoDecoder_GetOutputDescription(c->codec):OH_AudioCodec_GetOutputDescription(c->codec);
            if(f){OH_AVFormat_Destroy(f);c->reportedFormat=true;c->pendingOutput=index;return -2;}
        }
    }
    auto *buffer=c->video?OH_VideoDecoder_GetOutputBuffer(c->codec,index):OH_AudioCodec_GetOutputBuffer(c->codec,index);
    if(!buffer)return ERROR;OH_AVCodecBufferAttr attr{};if(OH_AVBuffer_GetBufferAttr(buffer,&attr)!=AV_ERR_OK)return ERROR;
    {std::lock_guard<std::mutex> lock(c->mutex);c->outputBuffers[index]=buffer;}
    *info={attr.offset,attr.size,attr.pts,ToAndroidFlags(attr.flags)};++c->decoded;
    if(c->decoded<=3)MEDIA_LOG("MEDIA_OUTPUT video=%{public}d index=%{public}u size=%{public}d pts=%{public}lld flags=%{public}u",c->video,index,attr.size,(long long)attr.pts,attr.flags);
    return index;
}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c,size_t index,size_t *size){
    if(size)*size=0;if(!c)return nullptr;std::lock_guard<std::mutex> lock(c->mutex);auto it=c->outputBuffers.find(index);
    if(it==c->outputBuffers.end())return nullptr;if(size)*size=std::max(0,OH_AVBuffer_GetCapacity(it->second));return OH_AVBuffer_GetAddr(it->second);
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c){
    if(!c)return nullptr;auto *f=c->video?OH_VideoDecoder_GetOutputDescription(c->codec):OH_AudioCodec_GetOutputDescription(c->codec);
    if(f&&!c->video){
        // OH reports the decoder MIME, Android reports the actual output MIME.
        // This output is real decoded S16 PCM, explicitly configured above.
        OH_AVFormat_SetStringValue(f,OH_MD_KEY_CODEC_MIME,"audio/raw");
        int32_t sample=0;if(OH_AVFormat_GetIntValue(f,OH_MD_KEY_AUDIO_SAMPLE_FORMAT,&sample)&&sample==SAMPLE_S16LE)OH_AVFormat_SetIntValue(f,"pcm-encoding",2);
    }
    int32_t channels=0,rate=0,width=0,height=0;if(f){OH_AVFormat_GetIntValue(f,OH_MD_KEY_AUD_CHANNEL_COUNT,&channels);OH_AVFormat_GetIntValue(f,OH_MD_KEY_AUD_SAMPLE_RATE,&rate);OH_AVFormat_GetIntValue(f,OH_MD_KEY_WIDTH,&width);OH_AVFormat_GetIntValue(f,OH_MD_KEY_HEIGHT,&height);}
    MEDIA_LOG("MEDIA_OUTPUT_FORMAT video=%{public}d channels=%{public}d rate=%{public}d width=%{public}d height=%{public}d",c->video,channels,rate,width,height);
    return f?new AMediaFormat(f):nullptr;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c,size_t index,bool render){
    if(!c)return INVALID;auto result=c->video?(render?OH_VideoDecoder_RenderOutputBuffer(c->codec,index):OH_VideoDecoder_FreeOutputBuffer(c->codec,index)):
        OH_AudioCodec_FreeOutputBuffer(c->codec,index);
    if(++c->released<=4)MEDIA_LOG("MEDIA_RELEASE video=%{public}d index=%{public}llu render=%{public}d result=%{public}d",c->video,(unsigned long long)index,render,result);
    static std::atomic<unsigned> errors{0};if(result!=AV_ERR_OK&&errors++<12)MEDIA_LOG("MEDIA_RELEASE_ERROR video=%{public}d index=%{public}llu result=%{public}d",c->video,(unsigned long long)index,result);
    return Status(result);
}
media_status_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *c,size_t index,int64_t timestamp){
    if(!c||!c->video)return INVALID;return Status(OH_VideoDecoder_RenderOutputBufferAtTime(c->codec,index,timestamp));
}
media_status_t AMediaCodec_setOutputSurface(AMediaCodec *c,void *surface){
    if(!c||!c->video||!surface)return INVALID;return Status(OH_VideoDecoder_SetSurface(c->codec,static_cast<OHNativeWindow *>(surface)));
}
// ImageReader/AHardwareBuffer import is a separate Vulkan path. Explicitly fail
// creation so GLES uses its SurfaceTexture path; never return a fake reader.
int AImageReader_newWithUsage(int32_t,int32_t,int32_t,uint64_t,int32_t,void **reader){if(reader)*reader=nullptr;return UNSUPPORTED;}
void AImageReader_delete(void *){}
int AImageReader_getWindow(void *,void **w){if(w)*w=nullptr;return UNSUPPORTED;}
int AImageReader_setImageListener(void *,void *){return UNSUPPORTED;}
int AImageReader_setBufferRemovedListener(void *,void *){return UNSUPPORTED;}
int AImageReader_acquireLatestImage(void *,void **image){if(image)*image=nullptr;return UNSUPPORTED;}
void AImage_delete(void *){}
void AImage_deleteAsync(void *,int){}
int AImage_getHardwareBuffer(const void *,void **b){if(b)*b=nullptr;return UNSUPPORTED;}
int AImage_getTimestamp(const void *,int64_t *t){if(t)*t=0;return UNSUPPORTED;}
int AImage_getWidth(const void *,int32_t *w){if(w)*w=0;return UNSUPPORTED;}
}
