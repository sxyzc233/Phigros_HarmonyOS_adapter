#pragma once
#include <cstdint>
extern "C" {
void *ReaVideoTextureCreate(uint32_t texture,void (*notify)(void *),void *context);
void ReaVideoTextureDestroy(void *state);
void *ReaVideoTextureWindow(void *state);
int ReaVideoTextureUpdate(void *state);
int ReaVideoTextureTransform(void *state,float *matrix);
int64_t ReaVideoTextureTimestamp(void *state);
int ReaVideoTextureAttach(void *state,uint32_t texture);
int ReaVideoTextureDetach(void *state);
int ReaVideoTextureSize(void *state,int width,int height);
}
