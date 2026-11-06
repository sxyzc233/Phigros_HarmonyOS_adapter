#include "video_surface.h"
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <hilog/log.h>
#include <atomic>
#include <mutex>

namespace {
struct Texture {
    OH_NativeImage *image=nullptr;OHNativeWindow *window=nullptr;
    void (*notify)(void *)=nullptr;void *context=nullptr;
    std::atomic<bool> released{false};std::atomic<uint64_t> available{0};
    std::mutex mutex;
};
void FrameAvailable(void *raw){auto *s=static_cast<Texture *>(raw);if(s->released)return;
    auto n=++s->available;if(n<=2||n%300==0)OH_LOG_Print(LOG_APP,LOG_INFO,0,"ReaMediaShim","MEDIA_TEXTURE_FRAME count=%{public}llu",(unsigned long long)n);
    if(s->notify)s->notify(s->context);
}
}
extern "C" void *ReaVideoTextureCreate(uint32_t texture,void (*notify)(void *),void *context){
    auto *s=new Texture;s->notify=notify;s->context=context;s->image=OH_NativeImage_Create(texture,0x8d65); // GL_TEXTURE_EXTERNAL_OES
    if(!s->image){delete s;return nullptr;}s->window=OH_NativeImage_AcquireNativeWindow(s->image);
    if(!s->window){OH_NativeImage_Destroy(&s->image);delete s;return nullptr;}
    OH_NativeImage_SetDropBufferMode(s->image,true);
    OH_OnFrameAvailableListener listener{s,FrameAvailable};int listenerResult=OH_NativeImage_SetOnFrameAvailableListener(s->image,listener);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"ReaMediaShim","MEDIA_TEXTURE_LISTENER result=%{public}d",listenerResult);
    OH_LOG_Print(LOG_APP,LOG_INFO,0,"ReaMediaShim","MEDIA_TEXTURE_CREATED texture=%{public}u",texture);return s;
}
extern "C" void ReaVideoTextureDestroy(void *raw){
    auto *s=static_cast<Texture *>(raw);if(!s||s->released.exchange(true))return;
    std::lock_guard<std::mutex> lock(s->mutex);OH_NativeImage_UnsetOnFrameAvailableListener(s->image);
    OH_NativeImage_Destroy(&s->image);if(s->window)OH_NativeWindow_DestroyNativeWindow(s->window);s->window=nullptr;
    // Small tombstone remains valid for any already-dispatched Java listener.
}
extern "C" void *ReaVideoTextureWindow(void *raw){auto *s=static_cast<Texture *>(raw);return s&&!s->released?s->window:nullptr;}
extern "C" int ReaVideoTextureUpdate(void *raw){auto *s=static_cast<Texture *>(raw);if(!s||s->released)return -1;
    std::lock_guard<std::mutex> lock(s->mutex);return s->image?OH_NativeImage_UpdateSurfaceImage(s->image):-1;}
extern "C" int ReaVideoTextureTransform(void *raw,float *m){auto *s=static_cast<Texture *>(raw);if(!s||s->released||!m)return -1;
    std::lock_guard<std::mutex> lock(s->mutex);return s->image?OH_NativeImage_GetTransformMatrixV2(s->image,m):-1;}
extern "C" int64_t ReaVideoTextureTimestamp(void *raw){auto *s=static_cast<Texture *>(raw);if(!s||s->released)return 0;
    std::lock_guard<std::mutex> lock(s->mutex);return s->image?OH_NativeImage_GetTimestamp(s->image):0;}
extern "C" int ReaVideoTextureAttach(void *raw,uint32_t texture){auto *s=static_cast<Texture *>(raw);if(!s||s->released)return -1;
    std::lock_guard<std::mutex> lock(s->mutex);return s->image?OH_NativeImage_AttachContext(s->image,texture):-1;}
extern "C" int ReaVideoTextureDetach(void *raw){auto *s=static_cast<Texture *>(raw);if(!s||s->released)return -1;
    std::lock_guard<std::mutex> lock(s->mutex);return s->image?OH_NativeImage_DetachContext(s->image):-1;}
extern "C" int ReaVideoTextureSize(void *raw,int width,int height){auto *s=static_cast<Texture *>(raw);if(!s||s->released||width<=0||height<=0)return -1;
    return OH_NativeWindow_NativeWindowHandleOpt(s->window,SET_BUFFER_GEOMETRY,width,height);}
