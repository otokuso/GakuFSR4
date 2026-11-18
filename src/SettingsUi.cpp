#include <windows.h>
#include <cmath>
#include <iterator>
#include <string>
#include "imgui.h"
#include "Preset.h"
#include "SettingsUi.h"

namespace gakufsr {
namespace {
GakuDesktopUiService desktop{};
std::wstring configPath;
bool draftEnabled=true;
int draftPreset=-1;
int draftSharpness=20;
const char* saveMessage=nullptr;

bool Save() {
    wchar_t sharpness[16]{};
    swprintf_s(sharpness,L"%d.%02d",draftSharpness/100,draftSharpness%100);
    const auto write=[](const wchar_t* key,const wchar_t* value) {
        return WritePrivateProfileStringW(L"FSR4",key,value,configPath.c_str())!=FALSE;
    };
    return (draftPreset<0 || write(L"Preset",FsrPresets[draftPreset].key)) &&
        write(L"Sharpness",sharpness) && write(L"Enabled",draftEnabled?L"1":L"0");
}
}

void InitSettingsUi(const GakuHostApi* host,const wchar_t* path,bool enabled,float ratio,float sharpness) {
    if(host->queryService(host->hostContext,GAKU_SERVICE_DESKTOP_UI,1,sizeof desktop,&desktop)!=GAKU_OK) return;
    configPath=path;
    draftEnabled=enabled;
    draftSharpness=static_cast<int>(std::lround(sharpness*100));
    draftPreset=-1;
    for(int i=0;i<static_cast<int>(std::size(FsrPresets));++i)
        if(FsrPresets[i].ratio==ratio) draftPreset=i;
}

void DrawSettings(void*) {
    if(!desktop.bind || configPath.empty()) return;
    const GakuImGuiAbi abi{IMGUI_VERSION_NUM,sizeof(ImGuiIO),sizeof(ImGuiStyle),
        sizeof(ImDrawVert),sizeof(ImDrawIdx),sizeof(ImWchar)};
    GakuDesktopUiBinding binding{};
    if(desktop.bind(&abi,sizeof binding,&binding)!=GAKU_OK ||
       !binding.imguiContext || !binding.alloc || !binding.free) return;
    ImGui::SetAllocatorFunctions(binding.alloc,binding.free,binding.allocatorUserData);
    auto* previous=ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(binding.imguiContext));
    ImGui::Checkbox("Enable FSR 4",&draftEnabled);
    if(ImGui::BeginCombo("Preset",draftPreset<0?"Custom ratio":FsrPresets[draftPreset].label)) {
        for(int i=0;i<static_cast<int>(std::size(FsrPresets));++i) {
            if(ImGui::Selectable(FsrPresets[i].label,draftPreset==i)) draftPreset=i;
            if(draftPreset==i) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SliderInt("Sharpness",&draftSharpness,0,100,"%d%%",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::Button("Save settings"))
        saveMessage=Save()?"Saved. Restart the game to apply.":"Could not save settings. Check the config file permissions.";
    ImGui::TextUnformatted(saveMessage?saveMessage:"Saved settings take effect after restarting the game.");
    ImGui::SetCurrentContext(previous);
}
}
