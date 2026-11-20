#include "FsrBridge.h"
#include "PluginRuntime.h"
#include "PresentHook.h"
#include "SettingsUi.h"

namespace gakufsr {
namespace {
GakuLogService logService{};
}

GakuRuntimeService runtime{};
GakuHookService hooks{};
std::atomic<bool> enabled{false};
bool frameGeneration=false;
PluginConfig options{};

void Log(const std::string& text, GakuLogLevel level) {
    if (logService.write) logService.write(logService.context,level,("[GakuFSR4] "+text).c_str());
}
GakuStatus Activate(const GakuHostApi* host,void** state) {
    if(!host||!state||host->abiVersion!=GAKU_ABI_VERSION||host->structSize<sizeof(GakuHostApi)||!host->queryService)
        return GAKU_E_VERSION;
    if(host->queryService(host->hostContext,GAKU_SERVICE_LOG,1,sizeof logService,&logService)!=GAKU_OK||
       host->queryService(host->hostContext,GAKU_SERVICE_RUNTIME,1,sizeof runtime,&runtime)!=GAKU_OK||
       host->queryService(host->hostContext,GAKU_SERVICE_HOOKS,1,sizeof hooks,&hooks)!=GAKU_OK||!hooks.acquireBatch)
        return GAKU_E_UNAVAILABLE;
    const auto dataPath=std::wstring(runtime.localRoot)+L"\\PluginData\\gakufsr4";
    auto config=std::wstring(runtime.localRoot)+L"\\Configs\\gakufsr4.ini";
    const auto status=ReadPluginConfig(config,options);
    if(status!=GAKU_OK) return status;
    InitSettingsUi(host,config.c_str(),options);
    if(!options.enabled) { *state=nullptr; return GAKU_OK; }
    if(!FSR4_Load(dataPath.c_str())) { Log("AMD FSR DLL loading failed",GAKU_LOG_ERROR); return GAKU_E_UNAVAILABLE; }
    frameGeneration=options.frameGeneration && FSR4_LoadFrameGeneration(dataPath.c_str());
    if(frameGeneration) frameGeneration=ConfigureFgViewport(config);
    if(frameGeneration) ConfigureFgDistortion(config);
    if(options.ratio==1.0f)
        Log("Native AA route: screen TAA pass; jitter sequence "+
            std::to_string(SelectJitterPhaseCount(options.jitterPhaseMode,options.ratio))+" phases");
    if(!CaptureGameSwapChain(hooks,frameGeneration))
        Log("DXGI factory hook unavailable; using a temporary swap chain for the Present hook",GAKU_LOG_WARNING);
    enabled=true;*state=nullptr;return GAKU_OK;
}
}

extern "C" __declspec(dllexport) GakuStatus GakuPlugin_Query(uint32_t abi,uint32_t size,GakuPluginDescriptor* out) {
    if(!out) return GAKU_E_INVALID_ARGUMENT;
    if(abi!=GAKU_ABI_VERSION) return GAKU_E_VERSION;
    if(size<sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;
    *out={GAKU_ABI_VERSION,sizeof(GakuPluginDescriptor),"gakufsr4","GakuFSR4","0.2.0",
        gakufsr::Activate,nullptr,gakufsr::Tick,nullptr,gakufsr::DrawSettings,gakufsr::Shutdown};
    return GAKU_OK;
}
