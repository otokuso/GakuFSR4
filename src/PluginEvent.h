#pragma once
#include <stdexcept>

using PluginEventFn=void(*)(void* nativeCommandBuffer,void* callback,int eventId,void* data);
inline PluginEventFn ResolvePluginEvent(void*(*resolve)(const char*)) {
    // Unity 6 strips the managed wrapper; the injected binding takes CommandBuffer.m_Ptr.
    auto fn=resolve?reinterpret_cast<PluginEventFn>(resolve(
        "UnityEngine.Rendering.CommandBuffer::IssuePluginEventAndDataInternal_Injected")):nullptr;
    if(!fn) throw std::runtime_error("Native plugin event binding unavailable");
    return fn;
}
inline void QueuePluginEvent(PluginEventFn fn,void* buffer,void* callback,int id,void* data) {
    if(!fn||!buffer||!callback||!data) throw std::runtime_error("Invalid plugin event arguments");
    fn(buffer,callback,id,data);
}
