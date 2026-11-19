#pragma once
#include "GakuPlugin.h"
#include <cstdint>

namespace gakufsr {
struct NativeAaDiagnostics {
    bool selected=false, beforePost=false, inputHdr=false;
    int jitterPhases=1024;
    uint64_t beforePostQueued=0, finalQueued=0, offscreenQueued=0, resetsQueued=0;
    float lastJitterX=0, lastJitterY=0;
};
NativeAaDiagnostics ReadNativeAaDiagnostics();
void RequestNativeAaHistoryReset();
void InitSettingsUi(const GakuHostApi* host,const wchar_t* path,bool enabled,float ratio,float sharpness,
    bool frameGeneration,bool nativeAaBeforePost,int jitterPhaseMode);
void DrawSettings(void*);
}
