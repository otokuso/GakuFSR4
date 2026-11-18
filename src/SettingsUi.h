#pragma once
#include "GakuPlugin.h"

namespace gakufsr {
void InitSettingsUi(const GakuHostApi* host,const wchar_t* path,bool enabled,float ratio,float sharpness,bool frameGeneration);
void DrawSettings(void*);
}
