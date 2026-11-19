#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "GakuPlugin.h"
#include "imgui.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
std::wstring root;
std::vector<std::string> messages;
int hookRequests=0;
int uiBindings=0;
GakuStatus BindUi(const GakuImGuiAbi* abi,uint32_t size,GakuDesktopUiBinding* out) {
    if(!abi || size!=sizeof(*out) || abi->versionNum!=IMGUI_VERSION_NUM ||
       abi->ioSize!=sizeof(ImGuiIO) || abi->styleSize!=sizeof(ImGuiStyle) ||
       abi->drawVertSize!=sizeof(ImDrawVert) || abi->drawIdxSize!=sizeof(ImDrawIdx) ||
       abi->wcharSize!=sizeof(ImWchar)) return GAKU_E_VERSION;
    *out={1,size,ImGui::GetCurrentContext(),
        [](size_t n,void*) {return std::malloc(n);},
        [](void* p,void*) {std::free(p);},nullptr};
    ++uiBindings;
    return GAKU_OK;
}
void Log(void*,GakuLogLevel,const char* text){ messages.emplace_back(text); }
void* NoGame(){return nullptr;}
GakuStatus Batch(void*,uint32_t,const GakuHookRequest*){ ++hookRequests;return GAKU_E_INTERNAL; }
GakuStatus Query(void*,const char* name,uint32_t version,uint32_t size,void* out){
    if(version!=1) return GAKU_E_VERSION;
    if(!strcmp(name,GAKU_SERVICE_LOG)&&size==sizeof(GakuLogService)){
        *static_cast<GakuLogService*>(out)={1,size,nullptr,Log};return GAKU_OK;
    }
    if(!strcmp(name,GAKU_SERVICE_RUNTIME)&&size==sizeof(GakuRuntimeService)){
        *static_cast<GakuRuntimeService*>(out)={1,size,root.c_str(),root.c_str(),root.c_str(),NoGame};return GAKU_OK;
    }
    if(!strcmp(name,GAKU_SERVICE_HOOKS)&&size==sizeof(GakuHookService)){
        *static_cast<GakuHookService*>(out)={1,size,nullptr,nullptr,nullptr,Batch};return GAKU_OK;
    }
    if(!strcmp(name,GAKU_SERVICE_DESKTOP_UI)&&size==sizeof(GakuDesktopUiService)){
        *static_cast<GakuDesktopUiService*>(out)={1,size,BindUi};return GAKU_OK;
    }
    return GAKU_E_UNAVAILABLE;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3) return 2;
    root=argv[2];
    auto module=LoadLibraryW(argv[1]);if(!module) return 3;
    auto query=reinterpret_cast<FnGakuPlugin_Query>(GetProcAddress(module,"GakuPlugin_Query"));
    if(!query) return 4;
    GakuPluginDescriptor descriptor{};
    if(query(99,sizeof descriptor,&descriptor)!=GAKU_E_VERSION||
       query(1,1,&descriptor)!=GAKU_E_SIZE||query(1,sizeof descriptor,nullptr)!=GAKU_E_INVALID_ARGUMENT||
       query(1,sizeof descriptor,&descriptor)!=GAKU_OK) return 5;
    GakuHostApi host{1,sizeof host,nullptr,Query,nullptr};void* state=nullptr;
    const GakuStatus activation=descriptor.activate(&host,&state);
    // Hosted Windows CI has no AMD runtime DLLs. The settings panel is still
    // initialized, but activation must fail gracefully in that environment.
    if(activation!=GAKU_OK&&activation!=GAKU_E_UNAVAILABLE) return 6;
    auto* imgui=ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui::GetIO().DisplaySize=ImVec2(640,480);
    unsigned char* pixels; int width,height;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    if(!descriptor.onDesktopUi) return 8;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(400,300));
    ImGui::Begin("Host panel");
    descriptor.onDesktopUi(state);
    ImGui::End();
    ImGui::Render();
    if(uiBindings!=1 || !ImGui::GetDrawData()->TotalVtxCount) return 9;
    ImGui::DestroyContext(imgui);
    bool failover=false;
    if(activation==GAKU_OK) {
        descriptor.onTick(state,0.016f);
        descriptor.onTick(state,0.016f);
        for(auto& message:messages)
            if(message.find("Missing il2cpp_domain_get")!=std::string::npos) failover=true;
        descriptor.shutdown(state);
    } else {
        for(auto& message:messages)
            if(message.find("AMD FSR DLL loading failed")!=std::string::npos) failover=true;
    }
    for(auto& message:messages) puts(message.c_str());
    if(!failover||hookRequests!=(activation==GAKU_OK?1:0)) return 7;
    puts("PASS: plugin ABI, settings panel draw and unavailable-runtime failover; no game started.");
    return 0;
}
