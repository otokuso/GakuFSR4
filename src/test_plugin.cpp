#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "GakuPlugin.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
std::wstring root;
std::vector<std::string> messages;
int hookRequests=0;
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
    if(descriptor.activate(&host,&state)!=GAKU_OK) return 6;
    descriptor.onTick(state,0.016f);
    descriptor.onTick(state,0.016f);
    bool stopped=false;
    for(auto& message:messages){ puts(message.c_str());if(message.find("Missing il2cpp_domain_get")!=std::string::npos) stopped=true; }
    if(!stopped||hookRequests) return 7;
    descriptor.shutdown(state);
    puts("PASS: plugin ABI, activation, unavailable-runtime failover; no game loaded or started.");
    return 0;
}
