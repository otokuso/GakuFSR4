#include "PluginEvent.h"
#include <cstring>
#include <cstdio>
int calls=0;
void *expectedBuffer,*expectedCallback,*expectedData;
void Fake(void* buffer,void* callback,int id,void* data) {
    if(buffer!=expectedBuffer||callback!=expectedCallback||id!=1||data!=expectedData)
        throw std::runtime_error("Incorrect native event ABI");
    ++calls;
}
void* Resolve(const char* name) {
    if(strcmp(name,"UnityEngine.Rendering.CommandBuffer::IssuePluginEventAndDataInternal_Injected"))
        throw std::runtime_error("Managed wrapper or incorrect icall requested");
    return reinterpret_cast<void*>(&Fake);
}
void* Missing(const char*) {return nullptr;}
int main() {
    int buffer=0,callback=0,data=0;
    expectedBuffer=&buffer;expectedCallback=&callback;expectedData=&data;
    auto fn=ResolvePluginEvent(Resolve);
    QueuePluginEvent(fn,&buffer,&callback,1,&data);
    if(calls!=1) return 1;
    bool rejected=false;
    try {ResolvePluginEvent(Missing);} catch(const std::runtime_error&) {rejected=true;}
    if(!rejected) return 2;
    rejected=false;
    try {QueuePluginEvent(fn,nullptr,&callback,1,&data);} catch(const std::runtime_error&) {rejected=true;}
    if(!rejected||calls!=1) return 3;
    puts("PASS: stripped-wrapper native binding, raw command-buffer ABI, missing binding and disposed-buffer rejection");
}
