#include <windows.h>
#include <cmath>
#include <iterator>
#include <string>
#include "imgui.h"
#include "Preset.h"
#include "PluginConfig.h"
#include "TaaJitter.h"
#include "FsrBridge.h"
#include "PresentRate.h"
#include "LiveTiming.h"
#include "DafpDiagnostics.h"
#include "SettingsUi.h"

namespace gakufsr {
namespace {
GakuDesktopUiService desktop{};
std::wstring configPath;
bool draftEnabled=true;
bool draftFrameGeneration=false;
int draftPreset=-1;
int draftSharpness=20;
int draftJitterPhaseMode=DefaultJitterPhaseMode;
const char* saveMessage=nullptr;
Stats currentStats{};
DafpDiagnostics currentDafp{};
ULONGLONG lastStatsRead=0;
ULONGLONG lastTimingRead=0;
LiveTimingSummary currentTiming{};

// AMD requires borderless fullscreen covering one display, or exclusive
// fullscreen. Client geometry avoids mistaking a large decorated window for FG
// fullscreen. Geometry alone cannot establish actual VRR/display-driver state.
int MonitorCoverage(void* window) {
    const HWND hwnd = static_cast<HWND>(window);
    if (!hwnd || !IsWindow(hwnd)) return -1;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return 0;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return -1;
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) return -1;
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return -1;
    constexpr LONG tolerance = 2;
    const auto& display = info.rcMonitor;
    return topLeft.x <= display.left + tolerance && topLeft.y <= display.top + tolerance &&
        bottomRight.x >= display.right - tolerance && bottomRight.y >= display.bottom - tolerance ? 1 : 0;
}

bool Save() {
    wchar_t sharpness[16]{};
    swprintf_s(sharpness,L"%d.%02d",draftSharpness/100,draftSharpness%100);
    const auto write=[](const wchar_t* key,const wchar_t* value) {
        return WritePrivateProfileStringW(L"FSR4",key,value,configPath.c_str())!=FALSE;
    };
    return (draftPreset<0 || write(L"Preset",FsrPresets[draftPreset].key)) &&
        write(L"Sharpness",sharpness) && write(L"FrameGeneration",draftFrameGeneration?L"1":L"0") &&
        write(L"JitterPhaseMode",draftJitterPhaseMode==2?L"2":draftJitterPhaseMode==1?L"1":L"0") &&
        write(L"Enabled",draftEnabled?L"1":L"0");
}

void DrawLiveTiming() {
    static PresentRate fps;
    fps.Sample(GetTickCount64(),ReadPresentCounts());
    if(fps.ready)
        ImGui::Text("Game FPS: %.1f  |  Present FPS (incl. FG): %.1f",fps.realFps,fps.totalFps);
    else
        ImGui::TextUnformatted("Game FPS: --  |  Present FPS (incl. FG): --");
    // Rolling window of real application Presents, including when FG is active.
    const ULONGLONG timingNow=GetTickCount64();
    if(!lastTimingRead || timingNow-lastTimingRead>=2000) {
        currentTiming=ReadLiveTiming();
        lastTimingRead=timingNow;
    }
    if(ImGui::CollapsingHeader("Live frame pacing / input timing",ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& p=currentTiming.present;
        if(p.samples)
            ImGui::Text("Game Present gap: median %.2f / p95 %.2f / p99 %.2f ms",p.medianMs,p.p95Ms,p.p99Ms);
        else
            ImGui::TextUnformatted("Game Present gap: -- (waiting for frames)");
        if(p.samples) ImGui::Text("Pacing jitter (stddev): %.2f ms  |  n=%zu",p.stddevMs,p.samples);
        const auto& input=currentTiming.input;
        if(input.samples)
            ImGui::Text("Input event -> Present start: median %.2f / p95 %.2f ms (n=%zu)",
                input.medianMs,input.p95Ms,input.samples);
        else if(currentTiming.inputHookActive)
            ImGui::TextUnformatted("Input event -> Present start: -- (press a key / move mouse)");
        else
            ImGui::TextUnformatted("Input event -> Present start: -- (game input hook unavailable)");
        ImGui::TextDisabled("App-side estimates, last 10 s; real game frames only.");
        ImGui::TextWrapped("Not display-frame pacing or full input-to-screen latency. "
            "For displayed frames and input-to-photon estimates, capture with PresentMon.");
    }
}

void DrawConfigControls() {
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
    if(ImGui::CollapsingHeader("Native AA / jitter (experimental)")) {
        constexpr const char* phaseModes[]={"Unity Halton (1024, legacy)","FSR recommended (all presets)","FSR only for Native AA (default)"};
        ImGui::Combo("Jitter phase mode",&draftJitterPhaseMode,phaseModes,3);
        ImGui::TextDisabled("Experimental. For Native AA the FSR phase count is 8; Unity's is 1024.");
    }
    if(ImGui::Button("Save settings"))
        saveMessage=Save()?"Saved. Restart the game to apply.":"Could not save settings. Check the config file permissions.";
    ImGui::TextUnformatted(saveMessage?saveMessage:"Saved settings take effect after restarting the game.");
}

void DrawRuntimeDiagnostics() {
    if (ImGui::CollapsingHeader("Runtime diagnostics",ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto aa=ReadNativeAaDiagnostics();
        if(aa.selected) {
            ImGui::TextUnformatted("Native AA stage: TAA pass (before bloom and tone mapping)");
            ImGui::Text("Jitter phases: %d",aa.jitterPhases);
            ImGui::Text("Native AA queued: early %llu / final %llu / offscreen %llu",
                (unsigned long long)aa.beforePostQueued,(unsigned long long)aa.finalQueued,
                (unsigned long long)aa.offscreenQueued);
            if(aa.beforePostQueued || aa.finalQueued || aa.offscreenQueued) {
                ImGui::Text("Last jitter: (%.3f, %.3f) px | input %s",
                    aa.lastJitterX,aa.lastJitterY,aa.inputHdr?"HDR flag set":"HDR flag unset");
                ImGui::Text("History resets queued: %llu",(unsigned long long)aa.resetsQueued);
            }
            ImGui::TextDisabled("Queued counts do not certify GPU dispatch success. Compare with FSR frames below.");
            if(ImGui::Button("Reset Native AA history on next frame")) RequestNativeAaHistoryReset();
        }
        static const char* lastFgSkip=nullptr;
        ULONGLONG now=GetTickCount64();
        if (!lastStatsRead || now-lastStatsRead>=2000) {
            FSR4_GetStats(&currentStats);
            FSR4_GetDafpDiagnostics(&currentDafp);
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
            if(s.fgGpuMs>0)
                ImGui::Text("FG GPU: %.2f ms (smoothed), VRAM: %.1f MiB",s.fgGpuMs,s.fgMemory/1048576.0);
            else
                ImGui::Text("FG GPU: --, VRAM: %.1f MiB",s.fgMemory/1048576.0);
            ImGui::Text("FG prepared: %llu, generated: %llu, real Presents: %llu",
                (unsigned long long)s.fgPrepared,(unsigned long long)s.fgGenerated,(unsigned long long)s.fgReal);
            ImGui::Text("FG skipped: %llu",(unsigned long long)s.fgSkipped);
            if(s.fgIntervalMs>0) ImGui::Text("FG source interval: %.2f ms",s.fgIntervalMs);
            else ImGui::TextUnformatted("FG source interval: --");
            if(FSR4_UsesNativeSwapchain()) ImGui::TextUnformatted("FG pacing: AMD SwapChain");
            ImGui::Text("FG last dispatch result: %d",s.fgLastResult);
        }
        if(ImGui::CollapsingHeader("DAFP status (diagnostic)",ImGuiTreeNodeFlags_DefaultOpen)) {
            const int coverage=MonitorCoverage(currentDafp.window);
            switch(EvaluateDafpEligibility(currentDafp,coverage)) {
            case DafpEligibility::Inactive:
                ImGui::TextUnformatted("DAFP: inactive (no recent native FG frames)"); break;
            case DafpEligibility::Blocked:
                ImGui::TextUnformatted("DAFP: prerequisites not met"); break;
            case DafpEligibility::Unknown:
                ImGui::TextUnformatted("DAFP: unknown (incomplete runtime data)"); break;
            case DafpEligibility::PossibleUnverified:
                ImGui::TextUnformatted("DAFP: possible (NOT confirmed active)"); break;
            }
            ImGui::Text("AMD native SwapChain: %s",currentDafp.nativeSwapchain?"yes":"no");
            if(currentDafp.swapchainMajor)
                ImGui::Text("SwapChain version: %u.x (%s)",currentDafp.swapchainMajor,
                    currentDafp.swapchainMajor>=4?"4.0.0+ required: yes":"4.0.0+ required: no");
            else ImGui::TextUnformatted("SwapChain version: -- (unknown)");
            ImGui::Text("Recent generated FG frames: %s",currentDafp.generatedRecent?"yes":"no");
            if(currentDafp.presentRecent)
                ImGui::Text("VSync disabled (SyncInterval=0): %s",currentDafp.syncInterval==0?"yes":"no");
            else ImGui::TextUnformatted("VSync disabled: -- (no recent Present)");
            ImGui::Text("Fullscreen monitor coverage: %s",coverage<0?"unknown":coverage?"yes":"no");
            ImGui::TextWrapped("Driver >= Adrenalin 26.10.1 and VRR window: not verified. "
                "AMD does not expose which pacing mode the driver selected. "
                "'Possible' only means the observable prerequisites are met.");
        }
        if(lastFgSkip) ImGui::TextWrapped("Last FG skip: %s",lastFgSkip);
        ImGui::TextDisabled("Snapshot refreshes every 2 seconds; some GPU timing values are rolling averages.");
    }
}
}

void InitSettingsUi(const GakuHostApi* host,const wchar_t* path,const PluginConfig& config) {
    if(host->queryService(host->hostContext,GAKU_SERVICE_DESKTOP_UI,1,sizeof desktop,&desktop)!=GAKU_OK) return;
    configPath=path;
    draftEnabled=config.enabled;
    draftFrameGeneration=config.frameGeneration;
    draftSharpness=static_cast<int>(std::lround(config.sharpness*100));
    draftJitterPhaseMode=config.jitterPhaseMode;
    draftPreset=-1;
    for(int i=0;i<static_cast<int>(std::size(FsrPresets));++i)
        if(FsrPresets[i].ratio==config.ratio) draftPreset=i;
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
    DrawLiveTiming();
    DrawConfigControls();
    DrawRuntimeDiagnostics();
    ImGui::SetCurrentContext(previous);
}
}
