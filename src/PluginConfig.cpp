#include "PluginConfig.h"
#include "PluginRuntime.h"
#include "FsrBridge.h"
#include "Preset.h"
#include <windows.h>
#include <cmath>
#include <cwchar>

namespace gakufsr {
GakuStatus ReadPluginConfig(const std::wstring& config, PluginConfig& result) {
    wchar_t providerVersion[64]{};
    GetPrivateProfileStringW(L"FSR4",L"ProviderVersion",L"",providerVersion,64,config.c_str());
    if(providerVersion[0]) {
        char asciiVersion[128]{};
        if(!WideCharToMultiByte(CP_UTF8,0,providerVersion,-1,asciiVersion,sizeof asciiVersion,nullptr,nullptr)||
           !FSR4_SetProviderVersion(asciiVersion)) {
            Log("Invalid ProviderVersion; use an available FSR 4.x version such as 4.1.2",GAKU_LOG_ERROR);
            return GAKU_E_INVALID_ARGUMENT;
        }
    }
    result.motionVectorYSign=GetPrivateProfileIntW(L"FSR4",L"MotionVectorYSign",-1,config.c_str());
    if(result.motionVectorYSign!=1&&result.motionVectorYSign!=-1) {
        Log("MotionVectorYSign must be +1 or -1",GAKU_LOG_ERROR);
        return GAKU_E_INVALID_ARGUMENT;
    }
    if(result.motionVectorYSign==1) Log("Experimental positive Y motion-vector scale enabled; compare against the default -1 sign",GAKU_LOG_WARNING);
    result.jitterXSign=GetPrivateProfileIntW(L"FSR4",L"JitterXSign",-1,config.c_str());
    result.jitterYSign=GetPrivateProfileIntW(L"FSR4",L"JitterYSign",-1,config.c_str());
    if((result.jitterXSign!=1&&result.jitterXSign!=-1)||(result.jitterYSign!=1&&result.jitterYSign!=-1)) {
        Log("JitterXSign/JitterYSign must be +1 or -1",GAKU_LOG_ERROR);
        return GAKU_E_INVALID_ARGUMENT;
    }
    if(result.jitterXSign!= -1||result.jitterYSign!=-1)
        Log("Experimental FSR jitter sign override enabled; compare against default -1/-1",GAKU_LOG_WARNING);
    result.jitterPhaseMode=GetPrivateProfileIntW(L"FSR4",L"JitterPhaseMode",DefaultJitterPhaseMode,config.c_str());
    if(result.jitterPhaseMode<0 || result.jitterPhaseMode>2) {
        Log("Invalid JitterPhaseMode; using Native AA FSR phases and Unity phases for upscaling",GAKU_LOG_WARNING);
        result.jitterPhaseMode=DefaultJitterPhaseMode;
    }
    result.enabled=GetPrivateProfileIntW(L"FSR4",L"Enabled",1,config.c_str())!=0;
    result.frameGeneration=GetPrivateProfileIntW(L"FSR4",L"FrameGeneration",0,config.c_str())!=0;
    wchar_t value[64];
    wchar_t* end=nullptr;
    if(GetPrivateProfileStringW(L"FSR4",L"Preset",L"",value,64,config.c_str())) {
        result.ratio=PresetRatio(value);
        if(result.ratio==0) {
            result.ratio=1.5f;
            Log("Unrecognized FSR4 Preset; using Quality",GAKU_LOG_WARNING);
        }
    } else {
        GetPrivateProfileStringW(L"FSR4",L"Ratio",L"1.5",value,64,config.c_str());
        result.ratio=wcstof(value,&end);
        if(!end||end==value||*end||!std::isfinite(result.ratio)||result.ratio<1||result.ratio>3) { Log("Invalid Ratio in configuration",GAKU_LOG_ERROR); return GAKU_E_INVALID_ARGUMENT; }
    }
    GetPrivateProfileStringW(L"FSR4",L"Sharpness",L"0.2",value,64,config.c_str());
    result.sharpness=wcstof(value,&end);
    if(!end||end==value||*end||!std::isfinite(result.sharpness)||result.sharpness<0||result.sharpness>1) return GAKU_E_INVALID_ARGUMENT;
    return GAKU_OK;
}

bool ConfigureFgViewport(const std::wstring& config) {
    const auto readViewport=[&](const wchar_t* name) {
        wchar_t text[32]{};
        const auto count=GetPrivateProfileStringW(L"FSR4",name,L"0",text,32,config.c_str());
        wchar_t* end=nullptr;
        const long value=wcstol(text,&end,10);
        return count<31 && end!=text && !*end && value>=0 && value<=16384 ? value : -1L;
    };
    const auto x=readViewport(L"FGViewportX"), y=readViewport(L"FGViewportY");
    const auto w=readViewport(L"FGViewportW"), h=readViewport(L"FGViewportH");
    if(x<0||y<0||w<0||h<0||((w==0)!=(h==0))||(!w&&(x||y))) {
        Log("Invalid FGViewport override; skipping frame generation",GAKU_LOG_WARNING);
        return false;
    }
    FSR4_SetFgViewportOverride(x,y,w,h);
    return true;
}

void ConfigureFgDistortion(const std::wstring& config) {
    float k1=0,k2=0,centerX=.5f,centerY=.5f;
    const auto readField=[&](const wchar_t* name,const wchar_t* fallback,float& result) {
        wchar_t text[64];
        const auto count=GetPrivateProfileStringW(L"FSR4",name,fallback,text,64,config.c_str());
        wchar_t* end=nullptr;
        result=wcstof(text,&end);
        return count<63 && end!=text && !*end && std::isfinite(result);
    };
    if(!readField(L"FGDistortionK1",L"0",k1) || !readField(L"FGDistortionK2",L"0",k2) ||
       !readField(L"FGDistortionCenterX",L"0.5",centerX) || !readField(L"FGDistortionCenterY",L"0.5",centerY) ||
       !FSR4_SetFgRadialDistortion(k1,k2,centerX,centerY)) {
        Log("Invalid FG radial distortion settings; using normal frame generation",GAKU_LOG_WARNING);
    }
}

}
