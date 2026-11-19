#include <windows.h>
#include <cmath>
#include <iterator>
#include <string>
#include "imgui.h"
#include "Preset.h"
#include "BridgeStats.h"
#include "PresentRate.h"
#include "SettingsUi.h"

extern "C" void FSR4_GetStats(Stats* out);
extern "C" const char* FSR4_GetLastFgSkipReason();

namespace gakufsr {
namespace {
GakuDesktopUiService desktop{};
std::wstring configPath;
bool draftEnabled=true;
bool draftFrameGeneration=false;
int draftPreset=-1;
int draftSharpness=20;
const char* saveMessage=nullptr;
Stats currentStats{};
ULONGLONG lastStatsRead=0;

bool Save() {
    wchar_t sharpness[16]{};
    swprintf_s(sharpness,L"%d.%02d",draftSharpness/100,draftSharpness%100);
    const auto write=[](const wchar_t* key,const wchar_t* value) {
        return WritePrivateProfileStringW(L"FSR4",key,value,configPath.c_str())!=FALSE;
    };
    return (draftPreset<0 || write(L"Preset",FsrPresets[draftPreset].key)) &&
        write(L"Sharpness",sharpness) && write(L"FrameGeneration",draftFrameGeneration?L"1":L"0") &&
        write(L"Enabled",draftEnabled?L"1":L"0");
}
}

void InitSettingsUi(const GakuHostApi* host,const wchar_t* path,bool enabled,float ratio,float sharpness,bool frameGeneration) {
    if(host->queryService(host->hostContext,GAKU_SERVICE_DESKTOP_UI,1,sizeof desktop,&desktop)!=GAKU_OK) return;
    configPath=path;
    draftEnabled=enabled;
    draftFrameGeneration=frameGeneration;
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
    static PresentRate fps;
    fps.Sample(GetTickCount64(),ReadPresentCounts());
    if(fps.ready)
        ImGui::Text("Game FPS: %.1f  |  Present FPS (incl. FG): %.1f",fps.realFps,fps.totalFps);
    else
        ImGui::TextUnformatted("Game FPS: --  |  Present FPS (incl. FG): --");
    ImGui::Checkbox("Enable FSR 4",&draftEnabled);
    ImGui::Checkbox("Frame Generation (2x)",&draftFrameGeneration);
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

    if (ImGui::CollapsingHeader("Runtime diagnostics",ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* lastFgSkip=nullptr;
        ULONGLONG now=GetTickCount64();
        if (!lastStatsRead || now-lastStatsRead>=2000) {
            FSR4_GetStats(&currentStats);
            lastFgSkip=FSR4_GetLastFgSkipReason();
            lastStatsRead=now;
        }
        const Stats& s=currentStats;
        if (!s.frames) {
            ImGui::TextUnformatted("FSR has not processed a frame yet (or is disabled).");
        } else {
            ImGui::Text("FSR provider: %s",s.provider[0]?s.provider:"not active");
            ImGui::Text("FSR GPU: %.2f ms (smoothed), CPU: %.2f ms",s.gpuMs,s.cpuMs);
            ImGui::Text("FSR VRAM: %.1f MiB",s.fsrMemory/1048576.0);
            ImGui::Text("Frames: %llu, backend failures: %u",(unsigned long long)s.frames,s.failures);
            ImGui::Separator();
            ImGui::Text("FG provider: %s",s.fgProvider[0]?s.fgProvider:"not active / unavailable");
            ImGui::Text("FG GPU: %.2f ms (smoothed), VRAM: %.1f MiB",s.fgGpuMs,s.fgMemory/1048576.0);
            ImGui::Text("FG prepared: %llu, generated: %llu, real Presents: %llu",
                (unsigned long long)s.fgPrepared,(unsigned long long)s.fgGenerated,(unsigned long long)s.fgReal);
            ImGui::Text("FG skipped: %llu, interval too short: %llu",
                (unsigned long long)s.fgSkipped,(unsigned long long)s.fgNotNeeded);
            ImGui::Text("FG interval: %.2f ms, pacing: %.2f ms",s.fgIntervalMs,s.fgPaceMs);
            ImGui::Text("FG last dispatch result: %d",s.fgLastResult);
        }
        if(lastFgSkip) ImGui::TextWrapped("Last FG skip: %s",lastFgSkip);
        ImGui::TextDisabled("Snapshot refreshes every 2 seconds; some GPU timing values are rolling averages.");
    }
    ImGui::SetCurrentContext(previous);
}
}
