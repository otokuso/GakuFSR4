#pragma once

#include "GakuPlugin.h"
#include "TaaJitter.h"
#include <string>

namespace gakufsr {
struct PluginConfig {
    bool enabled=true;
    bool frameGeneration=false;
    int jitterPhaseMode=DefaultJitterPhaseMode;
    int motionVectorYSign=-1;
    int jitterXSign=-1,jitterYSign=-1;
    float ratio=1.5f,sharpness=0.2f;
};

GakuStatus ReadPluginConfig(const std::wstring& path, PluginConfig& options);
bool ConfigureFgViewport(const std::wstring& path);
void ConfigureFgDistortion(const std::wstring& path);
}
