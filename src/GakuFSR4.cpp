// GPL-3.0: the D3D11/D3D12 bridge below is from marsattackk/GHPC-FSR4.
#include "../vendor/ghpc/bridge.cpp"
#include "GakuPlugin.h"
#include "FrameMath.h"
#include "Preset.h"
#include "SettingsUi.h"
#include "PluginEvent.h"
#include "ManagedRoot.h"
#include "MipBias.h"
#include "PrimaryCamera.h"
#include "TaaJitter.h"
#include <array>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <utility>

bool CaptureGameSwapChain(const GakuHookService& hooks);

namespace gakufsr {
using P = void*;
using M = const void*;
GakuLogService logService{};
GakuRuntimeService runtime{};
GakuHookService hooks{};
std::atomic<bool> enabled{false}, renderFailed{false};
bool bound=false, attempted=false;
bool frameGeneration=false;
bool fsrJitterCycle=false;
int motionVectorYSign=-1;
int jitterXSign=-1,jitterYSign=-1;
float ratio=1.5f, sharpness=0.2f;
std::wstring dataPath;

void Log(const std::string& text, GakuLogLevel level=GAKU_LOG_INFO) {
    if (logService.write) logService.write(logService.context,level,("[GakuFSR4] "+text).c_str());
}
void Stop(const std::string& why) {
    enabled=false;
    FSR4_StopFrameGenerationTimer();
    Log(why+"; using the game's renderer.",GAKU_LOG_WARNING);
}

struct ManagedFailure : std::runtime_error { using std::runtime_error::runtime_error; };

struct Api {
    P (*domain_get)();
    P* (*domain_get_assemblies)(P,size_t*);
    P (*assembly_get_image)(P);
    P (*class_from_name)(P,const char*,const char*);
    M (*class_get_methods)(P,P*);
    P (*class_get_parent)(P);
    const char* (*method_get_name)(M);
    uint32_t (*method_get_param_count)(M);
    P (*method_get_param)(M,uint32_t);
    char* (*type_get_name)(P);
    void (*free)(P);
    P (*class_get_field_from_name)(P,const char*);
    size_t (*field_get_offset)(P);
    int32_t (*class_value_size)(P,uint32_t*);
    P (*runtime_invoke)(M,P,P*,P*);
    P (*object_unbox)(P);
    P (*object_new)(P);
    P (*object_get_class)(P);
    GCHandle (*gchandle_new)(P,bool);
    P (*gchandle_get_target)(GCHandle);
    void (*gchandle_free)(GCHandle);
    P (*resolve_icall)(const char*);
    void (*format_exception)(P,char*,int);

    void Bind(HMODULE module) {
#define LOAD_API(n) n=reinterpret_cast<decltype(n)>(GetProcAddress(module,"il2cpp_" #n)); if(!n) throw std::runtime_error("Missing il2cpp_" #n)
        LOAD_API(domain_get); LOAD_API(domain_get_assemblies); LOAD_API(assembly_get_image);
        LOAD_API(class_from_name); LOAD_API(class_get_methods); LOAD_API(class_get_parent);
        LOAD_API(method_get_name); LOAD_API(method_get_param_count); LOAD_API(method_get_param);
        LOAD_API(type_get_name); LOAD_API(free); LOAD_API(class_get_field_from_name);
        LOAD_API(field_get_offset); LOAD_API(class_value_size); LOAD_API(runtime_invoke);
        LOAD_API(object_unbox); LOAD_API(object_new); LOAD_API(object_get_class);
        LOAD_API(gchandle_new); LOAD_API(gchandle_get_target); LOAD_API(gchandle_free);
        LOAD_API(resolve_icall);
        format_exception=reinterpret_cast<decltype(format_exception)>(GetProcAddress(module,"il2cpp_format_exception"));
#undef LOAD_API
    }
    P Class(const char* ns,const char* name) {
        size_t count=0;
        auto assemblies=domain_get_assemblies(domain_get(),&count);
        for(size_t i=0;i<count;++i)
            if(auto c=class_from_name(assembly_get_image(assemblies[i]),ns,name)) return c;
        throw std::runtime_error(std::string("Class not found: ")+ns+"."+name);
    }
    M Method(P c,const char* name,std::initializer_list<const char*> types) {
        M result=nullptr;
        for(P parent=c;parent && !result;parent=class_get_parent(parent)) {
            P iter=nullptr;
            while(auto m=class_get_methods(parent,&iter)) {
                if(strcmp(method_get_name(m),name) || method_get_param_count(m)!=types.size()) continue;
                bool match=true; uint32_t i=0;
                for(auto type:types) {
                    char* actual=type_get_name(method_get_param(m,i++));
                    if(!actual || strcmp(type,actual)) match=false;
                    if(actual) free(actual);
                }
                if(match) {
                    if(result) throw std::runtime_error(std::string("Ambiguous method: ")+name);
                    result=m;
                }
            }
        }
        if(!result) throw std::runtime_error(std::string("Method not found: ")+name);
        return result;
    }
    int Offset(P c,const char* name,bool value=false) {
        auto f=class_get_field_from_name(c,name);
        if(!f) throw std::runtime_error(std::string("Field not found: ")+name);
        auto offset=field_get_offset(f);
        if(offset<16 || offset>65536) throw std::runtime_error("Invalid field offset");
        return static_cast<int>(offset)-(value?16:0);
    }
    P Invoke(M m,P self,std::initializer_list<P> args={}) {
        P exception=nullptr;
        P result=runtime_invoke(m,self,const_cast<P*>(args.begin()),&exception);
        if(exception) {
            char detail[512]{};
            if(format_exception) format_exception(exception,detail,sizeof detail);
            detail[sizeof detail-1]='\0';
            throw ManagedFailure(std::string("Managed exception: ")+method_get_name(m)+": "+detail);
        }
        return result;
    }
    template<class T> T Value(M m,P self=nullptr,std::initializer_list<P> args={}) {
        P result=Invoke(m,self,args);
        if(!result) throw std::runtime_error("Missing boxed return value");
        return *static_cast<T*>(object_unbox(result));
    }
} api;

using Root = ManagedRoot<api>;
template<class T> T& At(P p,int offset) { return *reinterpret_cast<T*>(static_cast<char*>(p)+offset); }

struct Layout {
    int renderCamera, camera, type, target, baseType, hdr, aa, descriptor, pixelW,pixelH,scale,scaling,filter;
    int taaSettings, taaOffset, taaJitter, matrix, reset, taaData, resolve, xr;
    int descWidth,descHeight,descMsaa;
    int source,swap,postEnabled,rt,cameraStack,additionalType,renderer,commandBufferPtr,objectPtr;
    size_t cameraSize,descSize,rtiSize;
} field{};
struct Methods {
    M rtCtor,rtCreate,rtRelease,rtPtr,rtRandomWrite,rtHandleAlloc,rtHandleRelease;
    M width,height,targetDimension,targetSamples,targetDynamic,rtiTexture,rtiId,blit,blitCamera;
    PluginEventFn event=nullptr;
    M nearPlane,farPlane,fov,id,transform,position,frame,time,reversed,device,orthographic,dynamicResolution;
    M up,right,forward;
    M getBackBuffer;
    P rtClass,rtiClass;
} method{};

struct Surface {
    Root texture,handle;
    P native=nullptr;
    void Release() {
        if(handle.Get()) api.Invoke(method.rtHandleRelease,handle.Get());
        handle.Clear();
        texture.Clear(); native=nullptr;
    }
    void Create(int w,int h,int format,bool output=false) {
        texture=Root(api.object_new(method.rtClass));
        int depth=0,readWrite=1;
        api.Invoke(method.rtCtor,texture.Get(),{&w,&h,&depth,&format,&readWrite});
        if(output) { bool yes=true; api.Invoke(method.rtRandomWrite,texture.Get(),{&yes}); }
        if(!api.Value<bool>(method.rtCreate,texture.Get())) throw std::runtime_error("RenderTexture.Create failed");
        native=api.Value<P>(method.rtPtr,texture.Get());
        if(!native) throw std::runtime_error("Missing native texture");
        handle=Root(api.Invoke(method.rtHandleAlloc,nullptr,{texture.Get()}));
        if(!handle.Get()) throw std::runtime_error("RTHandles.Alloc failed");
    }
};
struct Resources {
    Surface color,depth,motion,output;
    int w=0,h=0,dw=0,dh=0;
    void Ensure(int rw,int rh,int displayW,int displayH) {
        if(w==rw&&h==rh&&dw==displayW&&dh==displayH) return;
        color.Release(); depth.Release(); motion.Release(); output.Release();
        w=h=dw=dh=0;
        color.Create(rw,rh,2); depth.Create(rw,rh,14); motion.Create(rw,rh,13);
        output.Create(displayW,displayH,2,true);
        w=rw;h=rh;dw=displayW;dh=displayH;
        Log("Allocated "+std::to_string(w)+"x"+std::to_string(h)+" -> "+std::to_string(dw)+"x"+std::to_string(dh));
    }
};
// The plugin is process-resident; never invoke IL2CPP from a CRT/DLL detach destructor.
Resources& resources=*new Resources;

using InitFn=void(*)(P,P,bool,P,M);
using TaaFn=void(*)(P,P,P,P,P,P,M);
using FinalFn=void(*)(P,P,P,M);
using ResolveBlitFn=void(*)(P,P,P,P,int,int,P,int,M);
InitFn originalInit=nullptr;
TaaFn originalTaa=nullptr;
FinalFn originalFinal=nullptr;
ResolveBlitFn originalResolveBlit=nullptr;

struct Capture {
    Root camera,motion;
    int frame=-1,id=0;
    float jitterX=0,jitterY=0;
};
Capture& capture=*new Capture;
struct History {
    int frame=-1,id=0,w=0,h=0;
    float fov=0,pos[3]{},forward[3]{};
} history;
std::atomic<uint64_t> submitted{0}, completed{0};
int configuredFrame=-1;
std::vector<int> configuredCameras;
PrimaryCamera primaryCamera;

void BlitCamera(P cmd,P src,P dst) {
    float mip=0; bool bilinear=false;
    api.Invoke(method.blitCamera,nullptr,{cmd,src,dst,&mip,&bilinear});
}
void Blit(P cmd,P texture,P destination,bool globalDepth=false) {
    std::vector<char> src(field.rtiSize),dst(field.rtiSize);
    using TexCtor=void(*)(P,P,M);
    using IdCtor=void(*)(P,int,M);
    if(globalDepth) {
        // The property is resolved when the command executes, after URP's depth copy.
        static int depthId=0;
        if(!depthId) {
            auto shader=api.Class("UnityEngine","Shader");
            auto prop=api.Method(shader,"PropertyToID",{"System.String"});
            using StringNew=P(*)(const char*);
            auto makeString=reinterpret_cast<StringNew>(GetProcAddress(static_cast<HMODULE>(runtime.getGameAssembly()),"il2cpp_string_new"));
            if(!makeString) throw std::runtime_error("Missing il2cpp_string_new");
            depthId=api.Value<int>(prop,nullptr,{makeString("_CameraDepthTexture")});
        }
        reinterpret_cast<IdCtor>(*static_cast<P const*>(method.rtiId))(src.data(),depthId,method.rtiId);
    } else reinterpret_cast<TexCtor>(*static_cast<P const*>(method.rtiTexture))(src.data(),texture,method.rtiTexture);
    reinterpret_cast<TexCtor>(*static_cast<P const*>(method.rtiTexture))(dst.data(),destination,method.rtiTexture);
    api.Invoke(method.blit,cmd,{src.data(),dst.data()});
}

struct Packet {
    FrameData data{};
    std::array<ComPtr<ID3D11Resource>,4> textures;
};
void __stdcall RenderEvent(int eventId,P data) {
    std::unique_ptr<Packet> packet(static_cast<Packet*>(data));
    if(eventId!=1||!packet) return;
    OnRenderEvent(1,&packet->data);
    FSR4_ViewsInUse(0);
    Stats stats{}; FSR4_GetStats(&stats);
    if(FSR4_Failed()) renderFailed=true;
    completed.fetch_add(1);
}

void Init(P camera,P additional,bool resolve,P cd,M mi) {
    originalInit(camera,additional,resolve,cd,mi);
    if(!enabled||renderFailed||!additional||!At<bool>(additional,field.postEnabled)) return;
    // URP fills CameraData.resolveFinalTarget and renderType after this call.
    try {
        if(!frame_math::SceneCamera(At<int>(cd,field.type)==1,At<int>(additional,field.additionalType)==0,
            resolve,At<bool>(cd,field.xr),api.Value<bool>(method.orthographic,camera))) return;
        P stack=At<P>(additional,field.cameraStack);
        if(stack&&At<int>(stack,api.Offset(api.object_get_class(stack),"_size"))>0) return;
        if(api.Value<bool>(method.dynamicResolution,camera)) return;
        int dw=At<int>(cd,field.pixelW),dh=At<int>(cd,field.pixelH);
        if(P target=At<P>(cd,field.target)) {
            if(!frame_math::SupportedTarget(api.Value<int>(method.targetDimension,target),
                api.Value<int>(method.targetSamples,target),api.Value<bool>(method.targetDynamic,target))) return;
            if(dw!=api.Value<int>(method.width,target)||dh!=api.Value<int>(method.height,target)) return;
        }
        if(dw<320||dh<240) return;
        const bool offscreen=At<P>(cd,field.target)!=nullptr;
        int w=frame_math::RenderSize(dw,ratio),h=frame_math::RenderSize(dh,ratio);
        int frame=api.Value<int>(method.frame),id=api.Value<int>(method.id,camera);
        if (!primaryCamera.Allow(id,frame)) return;
        if(frame!=configuredFrame) { configuredFrame=frame;configuredCameras.clear(); }
        if(configuredCameras.size()>=32) return;
        configuredCameras.push_back(id);
        P desc=static_cast<char*>(cd)+field.descriptor;
        auto matrix=reinterpret_cast<float*>(static_cast<char*>(cd)+field.matrix);
        const float oldJitterX=matrix[12],oldJitterY=matrix[13];
        const int oldW=At<int>(desc,field.descWidth),oldH=At<int>(desc,field.descHeight);
        const int oldAA=At<int>(cd,field.aa);
        At<int>(desc,field.descMsaa)=1;
        At<int>(cd,field.aa)=3;
        At<float>(cd,field.scale)=1/ratio;
        At<int>(cd,field.scaling)=ratio>1?1:0;
        // Keep the offscreen source at render resolution until the resolve hook.
        At<int>(cd,field.filter)=offscreen?0:2;
        At<int>(desc,field.descWidth)=w;
        At<int>(desc,field.descHeight)=h;
        auto taa=static_cast<char*>(cd)+field.taaSettings;
        const auto jitter=RepairTaaJitter(oldJitterX,oldJitterY,oldW,oldH,w,h,frame,
            At<int>(taa,field.taaOffset),At<float>(taa,field.taaJitter),
            fsrJitterCycle?FsrRecommendedJitterPhases(ratio):1024);
        matrix[12]=jitter.x;matrix[13]=jitter.y;
        static bool reported=false;
        if(!reported) { reported=true; Log("Configured first scene camera for FSR 4: "+
            std::to_string(w)+"x"+std::to_string(h)+" -> "+std::to_string(dw)+"x"+std::to_string(dh)+
            (At<P>(cd,field.target)?" (RenderTexture)":" (screen)")+
            "; original AA "+std::to_string(oldAA)+", jitter scale "+std::to_string(At<float>(taa,field.taaJitter))+
            ", jitter matrix offset "+std::to_string(oldJitterX)+","+std::to_string(oldJitterY)+
            " -> "+std::to_string(jitter.x)+","+std::to_string(jitter.y)); }
        static bool reportedBias=false;
        if(!reportedBias&&ratio>1) { reportedBias=true;Log("FSR recommended Mip LOD bias at ratio "+
            std::to_string(ratio)+": "+std::to_string(RecommendedFsrMipBias(ratio))+
            "; informational only (Unity URP may already apply a global Mip Bias)"); }
    } catch(const std::exception& e) { Stop(e.what()); }
}

void QueueFrame(P cmd,P cd,int w,int h,int dw,int dh);

bool LiveTexture(P texture) {
    return texture && At<P>(texture,field.objectPtr);
}

void Taa(P cmd,P material,P cd,P src,P dst,P motion,M mi) {
    if(!enabled||renderFailed||!motion||!At<bool>(cd,field.resolve)) {
        originalTaa(cmd,material,cd,src,dst,motion,mi); return;
    }
    static int blockedCamera=0,retryFrame=0;
    int frame=0,id=0;
    const bool offscreen=At<P>(cd,field.target)!=nullptr;
    const char* stage="camera validation";
    try {
        frame=api.Value<int>(method.frame);
        id=api.Value<int>(method.id,At<P>(cd,field.camera));
        if(frame!=configuredFrame||std::find(configuredCameras.begin(),configuredCameras.end(),id)==configuredCameras.end()) {
            originalTaa(cmd,material,cd,src,dst,motion,mi);return;
        }
        if(offscreen && (!frame_math::RetryCamera(id,blockedCamera,frame,retryFrame) ||
            !src || !dst || !LiveTexture(At<P>(src,field.rt)) ||
            !LiveTexture(At<P>(dst,field.rt)) || !LiveTexture(motion))) {
            originalTaa(cmd,material,cd,src,dst,motion,mi);
            return;
        }
        P desc=static_cast<char*>(cd)+field.descriptor;
        int w=At<int>(desc,field.descWidth),h=At<int>(desc,field.descHeight);
        auto matrix=reinterpret_cast<const float*>(static_cast<char*>(cd)+field.matrix);
        capture.camera=Root(At<P>(cd,field.camera)); capture.motion=Root(motion);
        capture.frame=frame;
        capture.id=id;
        capture.jitterX=frame_math::Jitter(matrix[12],w,jitterXSign);
        capture.jitterY=frame_math::Jitter(matrix[13],h,jitterYSign);
        if(offscreen && ratio==1.0f) {
            if(w!=At<int>(cd,field.pixelW) || h!=At<int>(cd,field.pixelH)) {
                originalTaa(cmd,material,cd,src,dst,motion,mi);
                return;
            }
            stage="allocate frame textures";
            if(!LiveTexture(resources.color.texture.Get()) || !LiveTexture(resources.output.texture.Get()))
                resources.w=0;
            resources.Ensure(w,h,w,h);
            stage="copy scene color";
            BlitCamera(cmd,src,resources.color.handle.Get());
            stage="queue FSR frame";
            QueueFrame(cmd,cd,w,h,w,h);
            stage="copy FSR output";
            BlitCamera(cmd,resources.output.handle.Get(),dst);
            if(blockedCamera==id && retryFrame>0) {
                retryFrame=0;
                Log("Resumed FSR 4 Native AA after a frame fallback");
            }
            static bool queued=false;
            if(!queued) { queued=true; Log("Queued first FSR 4 Native AA frame in the TAA pass"); }
        } else {
            BlitCamera(cmd,src,dst);
        }
        static bool reported=false;
        if(!reported) {
            reported=true;
            Log("Captured first camera motion vectors; FSR jitter "+std::to_string(capture.jitterX)+","+
                std::to_string(capture.jitterY)+" px");
        }
        return;
    } catch(const ManagedFailure& e) {
        if(offscreen && id!=0) {
            blockedCamera=id;
            retryFrame=frame+30;
            history.frame=-1;
            capture.frame=-1;
            Log(std::string(stage)+": "+e.what()+"; using game TAA for this camera before retrying",GAKU_LOG_WARNING);
        } else Stop(e.what());
    } catch(const std::exception& e) { Stop(e.what()); }
    originalTaa(cmd,material,cd,src,dst,motion,mi);
}

void QueueFrame(P cmd,P cd,int w,int h,int dw,int dh) {
    Blit(cmd,nullptr,resources.depth.texture.Get(),true);
    Blit(cmd,capture.motion.Get(),resources.motion.texture.Get());
    // A failed FSR dispatch leaves a valid spatially scaled frame, never uninitialized memory.
    Blit(cmd,resources.color.texture.Get(),resources.output.texture.Get());
    auto packet=std::make_unique<Packet>();
    auto& fd=packet->data;
    fd.color=resources.color.native;fd.depth=resources.depth.native;
    fd.motion=resources.motion.native;fd.output=resources.output.native;
    // On the screen-camera path this command is recorded in RenderFinalPass,
    // before later overlay UI may be drawn to the game's swap chain. Expose the
    // pre-overlay FSR output for HUDless/UI composition validation. Whether
    // every in-game canvas is absent must still be verified in the target game.
    fd.hudless=frameGeneration && !At<P>(cd,field.target) ? fd.output : nullptr;
    P pointers[]{fd.color,fd.depth,fd.motion,fd.output};
    for(int i=0;i<4;++i) packet->textures[i]=static_cast<ID3D11Resource*>(pointers[i]);
    fd.renderW=w;fd.renderH=h;fd.displayW=dw;fd.displayH=dh;
    fd.maxRenderW=w;fd.maxRenderH=h;
    fd.jitterX=capture.jitterX;fd.jitterY=capture.jitterY;
    fd.mvScaleX=-static_cast<float>(w);fd.mvScaleY=frame_math::MotionVectorYScale(h,motionVectorYSign);
    fd.frameTimeMs=(std::max)(0.1f,api.Value<float>(method.time)*1000);
    fd.cameraNear=api.Value<float>(method.nearPlane,capture.camera.Get());
    fd.cameraFar=api.Value<float>(method.farPlane,capture.camera.Get());
    float fov=api.Value<float>(method.fov,capture.camera.Get());
    // The game exposure is not captured by this integration. Zero tells the
    // bridge to retain SDK auto exposure rather than fabricating 1.0.
    fd.fovV=fov*0.01745329252f;fd.sharpness=sharpness;fd.exposure=0;
    fd.createFlags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if(At<P>(cd,field.target)&&At<bool>(cd,field.hdr)) fd.createFlags|=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if(api.Value<bool>(method.reversed)) fd.createFlags|=FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    using Vec3=std::array<float,3>;
    P transform=api.Invoke(method.transform,capture.camera.Get());
    auto pos=api.Value<Vec3>(method.position,transform);
    auto forward=api.Value<Vec3>(method.forward,transform);
    fd.reset=frame_math::Reset(capture.id,history.id,capture.frame,history.frame,fov,history.fov,pos.data(),history.pos)||
        history.w!=w||history.h!=h||frame_math::RotationJump(forward.data(),history.forward);
    auto taa=static_cast<char*>(cd)+field.taaSettings;
    fd.reset|=At<int>(taa,field.reset)>0;
    if(frameGeneration) {
        fd.fg=1;
        std::copy(pos.begin(),pos.end(),fd.camPos);
        std::copy(forward.begin(),forward.end(),fd.camFwd);
        for(auto [getter,dst]:{std::pair{method.up,fd.camUp},std::pair{method.right,fd.camRight}}) {
            auto axis=api.Value<Vec3>(getter,transform);
            std::copy(axis.begin(),axis.end(),dst);
        }
    }
    QueuePluginEvent(method.event,At<P>(cmd,field.commandBufferPtr),
        reinterpret_cast<P>(&RenderEvent),1,packet.get());
    packet.release();submitted.fetch_add(1);
    history.id=capture.id;history.frame=capture.frame;history.fov=fov;history.w=w;history.h=h;
    std::copy(pos.begin(),pos.end(),history.pos);
    std::copy(forward.begin(),forward.end(),history.forward);
}

void Final(P self,P cmd,P rd,M mi) {
    P cd=static_cast<char*>(rd)+field.renderCamera;
    struct Restore {
        P cd,self,source=nullptr;
        bool swap=false,active=false;
        std::vector<char> saved;
        ~Restore() {
            if(!active) return;
            memcpy(cd,saved.data(),saved.size());
            At<P>(self,field.source)=source;At<bool>(self,field.swap)=swap;
        }
    } restore{cd,self};
    try {
        if(enabled && !renderFailed && !At<P>(cd,field.target) && At<bool>(cd,field.resolve) && capture.frame==api.Value<int>(method.frame) &&
           capture.id==api.Value<int>(method.id,At<P>(cd,field.camera))) {
            P source=At<P>(self,field.source);
            if(At<bool>(self,field.swap)) {
                P renderer=At<P>(cd,field.renderer);
                if(!method.getBackBuffer) method.getBackBuffer=api.Method(api.object_get_class(renderer),"GetCameraColorBackBuffer",{"UnityEngine.Rendering.CommandBuffer"});
                source=api.Invoke(method.getBackBuffer,renderer,{cmd});
            }
            P sourceTexture=source?At<P>(source,field.rt):nullptr;
            if(!sourceTexture) throw std::runtime_error("Final source has no RenderTexture");
            int w=api.Value<int>(method.width,sourceTexture),h=api.Value<int>(method.height,sourceTexture);
            int dw=At<int>(cd,field.pixelW),dh=At<int>(cd,field.pixelH);
            P desc=static_cast<char*>(cd)+field.descriptor;
            if(w!=At<int>(desc,field.descWidth)||h!=At<int>(desc,field.descHeight))
                throw std::runtime_error("Scaled RTHandles are not supported by this integration");
            resources.Ensure(w,h,dw,dh);
            BlitCamera(cmd,source,resources.color.handle.Get());
            QueueFrame(cmd,cd,w,h,dw,dh);

            restore.saved.assign(static_cast<char*>(cd),static_cast<char*>(cd)+field.cameraSize);
            restore.source=At<P>(self,field.source);restore.swap=At<bool>(self,field.swap);
            restore.active=true;
            At<P>(self,field.source)=resources.output.handle.Get();At<bool>(self,field.swap)=false;
            At<int>(desc,field.descWidth)=dw;At<int>(desc,field.descHeight)=dh;
            At<float>(cd,field.scale)=1;At<int>(cd,field.scaling)=0;At<int>(cd,field.aa)=0;
        }
    } catch(const std::exception& e) { Stop(e.what()); }
    originalFinal(self,cmd,rd,mi);
}

void ResolveBlit(P cmd,P cd,P src,P dst,int load,int store,P material,int pass,M mi) {
    P input=src;
    try {
        if(enabled && !renderFailed && ratio>1 && capture.frame>=0 &&
           At<P>(cd,field.target) && At<bool>(cd,field.resolve) &&
           At<P>(cd,field.camera)==capture.camera.Get() &&
           capture.frame==api.Value<int>(method.frame) && src && dst) {
            P texture=At<P>(src,field.rt);
            P desc=static_cast<char*>(cd)+field.descriptor;
            int w=At<int>(desc,field.descWidth),h=At<int>(desc,field.descHeight);
            int dw=At<int>(cd,field.pixelW),dh=At<int>(cd,field.pixelH);
            if(LiveTexture(texture) && LiveTexture(capture.motion.Get()) &&
               frame_math::UpscaleSource(api.Value<int>(method.width,texture),
                   api.Value<int>(method.height,texture),w,h,dw,dh)) {
                if(!LiveTexture(resources.color.texture.Get()) || !LiveTexture(resources.output.texture.Get()))
                    resources.w=0;
                resources.Ensure(w,h,dw,dh);
                BlitCamera(cmd,src,resources.color.handle.Get());
                QueueFrame(cmd,cd,w,h,dw,dh);
                input=resources.output.handle.Get();
                capture.frame=-1;
                static bool reported=false;
                if(!reported) { reported=true; Log("Queued first FSR 4 upscale in camera resolve: "+
                    std::to_string(w)+"x"+std::to_string(h)+" -> "+std::to_string(dw)+"x"+std::to_string(dh)); }
            }
        }
    } catch(const ManagedFailure& e) {
        capture.frame=-1;
        history.frame=-1;
        static bool reported=false;
        if(!reported) { reported=true; Log(std::string("Camera resolve fallback: ")+e.what(),GAKU_LOG_WARNING); }
    } catch(const std::exception& e) { Stop(e.what()); }
    originalResolveBlit(cmd,cd,input,dst,load,store,material,pass,mi);
}

void Bind() {
    api.Bind(static_cast<HMODULE>(runtime.getGameAssembly()));
    P cd=api.Class("UnityEngine.Rendering.Universal","CameraData");
    P rd=api.Class("UnityEngine.Rendering.Universal","RenderingData");
    P desc=api.Class("UnityEngine","RenderTextureDescriptor");
    P pass=api.Class("UnityEngine.Rendering.Universal","PostProcessPass");
    P taa=api.Class("UnityEngine.Rendering.Universal","TemporalAA");
    P pipeline=api.Class("UnityEngine.Rendering.Universal","UniversalRenderPipeline");
    P additional=api.Class("UnityEngine.Rendering.Universal","UniversalAdditionalCameraData");
    P handle=api.Class("UnityEngine.Rendering","RTHandle");
    P cmd=api.Class("UnityEngine.Rendering","CommandBuffer");
    P camera=api.Class("UnityEngine","Camera");
    P texture=api.Class("UnityEngine","Texture");
    method.rtClass=api.Class("UnityEngine","RenderTexture");
    method.rtiClass=api.Class("UnityEngine.Rendering","RenderTargetIdentifier");
#define VF(member,name) field.member=api.Offset(cd,name,true)
    VF(camera,"camera");VF(type,"cameraType");VF(target,"targetTexture");VF(baseType,"renderType");
    VF(hdr,"isHdrEnabled");VF(aa,"antialiasing");VF(descriptor,"cameraTargetDescriptor");
    VF(pixelW,"pixelWidth");VF(pixelH,"pixelHeight");VF(scale,"renderScale");
    VF(scaling,"imageScalingMode");VF(filter,"upscalingFilter");VF(taaSettings,"taaSettings");
    VF(matrix,"m_JitterMatrix");VF(taaData,"taaPersistentData");VF(resolve,"resolveFinalTarget");VF(xr,"xrRendering");VF(renderer,"renderer");
#undef VF
    field.renderCamera=api.Offset(rd,"cameraData",true);
    field.descWidth=api.Offset(desc,"<width>k__BackingField",true);
    field.descHeight=api.Offset(desc,"<height>k__BackingField",true);
    field.descMsaa=api.Offset(desc,"<msaaSamples>k__BackingField",true);
    field.source=api.Offset(pass,"m_Source");field.swap=api.Offset(pass,"m_UseSwapBuffer");
    field.postEnabled=api.Offset(additional,"m_RenderPostProcessing");field.rt=api.Offset(handle,"m_RT");
    field.cameraStack=api.Offset(additional,"m_Cameras");
    field.additionalType=api.Offset(additional,"m_CameraType");
    field.commandBufferPtr=api.Offset(cmd,"m_Ptr");
    field.objectPtr=api.Offset(api.Class("UnityEngine","Object"),"m_CachedPtr");
    auto settingsField=api.class_get_field_from_name(cd,"taaSettings");
    using FieldTypeFn=P(*)(P);using ClassTypeFn=P(*)(P);
    auto ft=reinterpret_cast<FieldTypeFn>(GetProcAddress(static_cast<HMODULE>(runtime.getGameAssembly()),"il2cpp_field_get_type"));
    auto ct=reinterpret_cast<ClassTypeFn>(GetProcAddress(static_cast<HMODULE>(runtime.getGameAssembly()),"il2cpp_class_from_type"));
    if(!ft||!ct) throw std::runtime_error("Missing field type API");
    P settings=ct(ft(settingsField));field.reset=api.Offset(settings,"resetHistoryFrames",true);
    field.taaOffset=api.Offset(settings,"jitterFrameCountOffset",true);
    field.taaJitter=api.Offset(settings,api.class_get_field_from_name(settings,"m_JitterScale")?"m_JitterScale":"jitterScale",true);
    field.cameraSize=api.class_value_size(cd,nullptr);field.descSize=api.class_value_size(desc,nullptr);
    field.rtiSize=api.class_value_size(method.rtiClass,nullptr);
    if(field.cameraSize<256||field.cameraSize>4096||field.rtiSize>256||field.rtiSize<16)
        throw std::runtime_error("Unexpected URP value type layout");
    method.rtCtor=api.Method(method.rtClass,".ctor",{"System.Int32","System.Int32","System.Int32","UnityEngine.RenderTextureFormat","UnityEngine.RenderTextureReadWrite"});
    method.rtCreate=api.Method(method.rtClass,"Create",{});method.rtRelease=api.Method(method.rtClass,"Release",{});
    method.rtRandomWrite=api.Method(method.rtClass,"set_enableRandomWrite",{"System.Boolean"});
    method.rtPtr=api.Method(texture,"GetNativeTexturePtr",{});
    method.width=api.Method(method.rtClass,"get_width",{});method.height=api.Method(method.rtClass,"get_height",{});
    method.targetDimension=api.Method(method.rtClass,"get_dimension",{});
    method.targetSamples=api.Method(method.rtClass,"get_antiAliasing",{});
    method.targetDynamic=api.Method(method.rtClass,"get_useDynamicScale",{});
    method.rtHandleAlloc=api.Method(api.Class("UnityEngine.Rendering","RTHandles"),"Alloc",{"UnityEngine.RenderTexture"});
    method.rtHandleRelease=api.Method(handle,"Release",{});
    method.rtiTexture=api.Method(method.rtiClass,".ctor",{"UnityEngine.Texture"});
    method.rtiId=api.Method(method.rtiClass,".ctor",{"System.Int32"});
    method.blit=api.Method(cmd,"Blit",{"UnityEngine.Rendering.RenderTargetIdentifier","UnityEngine.Rendering.RenderTargetIdentifier"});
    method.blitCamera=api.Method(api.Class("UnityEngine.Rendering","Blitter"),"BlitCameraTexture",
        {"UnityEngine.Rendering.CommandBuffer","UnityEngine.Rendering.RTHandle","UnityEngine.Rendering.RTHandle","System.Single","System.Boolean"});
    method.event=ResolvePluginEvent(api.resolve_icall);
    method.nearPlane=api.Method(camera,"get_nearClipPlane",{});method.farPlane=api.Method(camera,"get_farClipPlane",{});
    method.fov=api.Method(camera,"get_fieldOfView",{});method.id=api.Method(api.Class("UnityEngine","Object"),"GetInstanceID",{});
    method.orthographic=api.Method(camera,"get_orthographic",{});
    method.dynamicResolution=api.Method(camera,"get_allowDynamicResolution",{});
    method.transform=api.Method(api.Class("UnityEngine","Component"),"get_transform",{});
    auto transform=api.Class("UnityEngine","Transform");
    method.position=api.Method(transform,"get_position",{});
    method.forward=api.Method(transform,"get_forward",{});
    if(frameGeneration) {
        method.up=api.Method(transform,"get_up",{});
        method.right=api.Method(transform,"get_right",{});
    }
    method.frame=api.Method(api.Class("UnityEngine","Time"),"get_frameCount",{});
    method.time=api.Method(api.Class("UnityEngine","Time"),"get_unscaledDeltaTime",{});
    P sys=api.Class("UnityEngine","SystemInfo");
    method.reversed=api.Method(sys,"get_usesReversedZBuffer",{});method.device=api.Method(sys,"get_graphicsDeviceType",{});
    if(api.Value<int>(method.device)!=2) throw std::runtime_error("This build supports the game's Direct3D 11 renderer only");
    M init=api.Method(pipeline,"InitializeCameraData",{"UnityEngine.Camera","UnityEngine.Rendering.Universal.UniversalAdditionalCameraData","System.Boolean","UnityEngine.Rendering.Universal.CameraData&"});
    M taaPass=api.Method(taa,"ExecutePass",{"UnityEngine.Rendering.CommandBuffer","UnityEngine.Material","UnityEngine.Rendering.Universal.CameraData&","UnityEngine.Rendering.RTHandle","UnityEngine.Rendering.RTHandle","UnityEngine.RenderTexture"});
    M final=api.Method(pass,"RenderFinalPass",{"UnityEngine.Rendering.CommandBuffer","UnityEngine.Rendering.Universal.RenderingData&"});
    M resolveBlit=api.Method(api.Class("UnityEngine.Rendering.Universal","RenderingUtils"),"FinalBlit",
        {"UnityEngine.Rendering.CommandBuffer","UnityEngine.Rendering.Universal.CameraData&",
         "UnityEngine.Rendering.RTHandle","UnityEngine.Rendering.RTHandle","UnityEngine.Rendering.RenderBufferLoadAction",
         "UnityEngine.Rendering.RenderBufferStoreAction","UnityEngine.Material","System.Int32"});
    GakuHookRequest requests[]{
        {*static_cast<P const*>(init),reinterpret_cast<P>(&Init),reinterpret_cast<P*>(&originalInit)},
        {*static_cast<P const*>(taaPass),reinterpret_cast<P>(&Taa),reinterpret_cast<P*>(&originalTaa)},
        {*static_cast<P const*>(final),reinterpret_cast<P>(&Final),reinterpret_cast<P*>(&originalFinal)},
        {*static_cast<P const*>(resolveBlit),reinterpret_cast<P>(&ResolveBlit),reinterpret_cast<P*>(&originalResolveBlit)},
    };
    if(hooks.acquireBatch(hooks.context,4,requests)!=GAKU_OK) throw std::runtime_error("Could not acquire URP hook batch");
    Log("URP hooks installed. FSR 4 upscale ratio "+std::to_string(ratio)+
        (frameGeneration?". Frame generation enabled.":". Frame generation disabled."));
    bound=true;
}

GakuStatus Activate(const GakuHostApi* host,P* state) {
    if(!host||!state||host->abiVersion!=GAKU_ABI_VERSION||host->structSize<sizeof(GakuHostApi)||!host->queryService)
        return GAKU_E_VERSION;
    if(host->queryService(host->hostContext,GAKU_SERVICE_LOG,1,sizeof logService,&logService)!=GAKU_OK||
       host->queryService(host->hostContext,GAKU_SERVICE_RUNTIME,1,sizeof runtime,&runtime)!=GAKU_OK||
       host->queryService(host->hostContext,GAKU_SERVICE_HOOKS,1,sizeof hooks,&hooks)!=GAKU_OK||!hooks.acquireBatch)
        return GAKU_E_UNAVAILABLE;
    dataPath=std::wstring(runtime.localRoot)+L"\\PluginData\\gakufsr4";
    auto config=std::wstring(runtime.localRoot)+L"\\Configs\\gakufsr4.ini";
    wchar_t providerVersion[64]{};
    GetPrivateProfileStringW(L"FSR4",L"ProviderVersion",L"",providerVersion,64,config.c_str());
    if(providerVersion[0]) {
        char asciiVersion[128]{};
        if(!WideCharToMultiByte(CP_UTF8,0,providerVersion,-1,asciiVersion,sizeof asciiVersion,nullptr,nullptr)||
           !FSR4_SetProviderVersion(asciiVersion)) {
            Log("Invalid ProviderVersion; use an available FSR 4.x version such as 4.1.2",GAKU_LOG_ERROR);
            return GAKU_E_INVALID_ARGUMENT;
        }
    }
    motionVectorYSign=GetPrivateProfileIntW(L"FSR4",L"MotionVectorYSign",-1,config.c_str());
    if(motionVectorYSign!=1&&motionVectorYSign!=-1) {
        Log("MotionVectorYSign must be +1 or -1",GAKU_LOG_ERROR);
        return GAKU_E_INVALID_ARGUMENT;
    }
    if(motionVectorYSign==1) Log("Experimental positive Y motion-vector scale enabled; compare against the default -1 sign",GAKU_LOG_WARNING);
    jitterXSign=GetPrivateProfileIntW(L"FSR4",L"JitterXSign",-1,config.c_str());
    jitterYSign=GetPrivateProfileIntW(L"FSR4",L"JitterYSign",-1,config.c_str());
    if((jitterXSign!=1&&jitterXSign!=-1)||(jitterYSign!=1&&jitterYSign!=-1)) {
        Log("JitterXSign/JitterYSign must be +1 or -1",GAKU_LOG_ERROR);
        return GAKU_E_INVALID_ARGUMENT;
    }
    if(jitterXSign!= -1||jitterYSign!=-1)
        Log("Experimental FSR jitter sign override enabled; compare against default -1/-1",GAKU_LOG_WARNING);
    const auto cycleMode=GetPrivateProfileIntW(L"FSR4",L"JitterPhaseMode",0,config.c_str());
    fsrJitterCycle=cycleMode==1;
    if(cycleMode>1) Log("Invalid JitterPhaseMode; using the normal Unity jitter cycle",GAKU_LOG_WARNING);
    if(fsrJitterCycle) Log("Experimental FSR jitter phase cycle enabled",GAKU_LOG_WARNING);
    const bool configEnabled=GetPrivateProfileIntW(L"FSR4",L"Enabled",1,config.c_str())!=0;
    const bool configFg=GetPrivateProfileIntW(L"FSR4",L"FrameGeneration",0,config.c_str())!=0;
    wchar_t value[64];
    wchar_t* end=nullptr;
    if(GetPrivateProfileStringW(L"FSR4",L"Preset",L"",value,64,config.c_str())) {
        ratio=PresetRatio(value);
        if(ratio==0) {
            ratio=1.5f;
            Log("Unrecognized FSR4 Preset; using Quality",GAKU_LOG_WARNING);
        }
    } else {
        GetPrivateProfileStringW(L"FSR4",L"Ratio",L"1.5",value,64,config.c_str());
        ratio=wcstof(value,&end);
        if(!end||end==value||*end||!std::isfinite(ratio)||ratio<1||ratio>3) { Log("Invalid Ratio in configuration",GAKU_LOG_ERROR); return GAKU_E_INVALID_ARGUMENT; }
    }
    GetPrivateProfileStringW(L"FSR4",L"Sharpness",L"0.2",value,64,config.c_str());
    sharpness=wcstof(value,&end);
    if(!end||end==value||*end||!std::isfinite(sharpness)||sharpness<0||sharpness>1) return GAKU_E_INVALID_ARGUMENT;
    InitSettingsUi(host,config.c_str(),configEnabled,ratio,sharpness,configFg);
    if(!configEnabled) { *state=nullptr; return GAKU_OK; }
    if(!FSR4_Load(dataPath.c_str())) { Log("AMD FSR DLL loading failed",GAKU_LOG_ERROR); return GAKU_E_UNAVAILABLE; }
    frameGeneration=configFg && FSR4_LoadFrameGeneration(dataPath.c_str());
    if(frameGeneration) {
        const auto readViewport=[&](const wchar_t* name) {
            wchar_t text[32]{};
            const auto count=GetPrivateProfileStringW(L"FSR4",name,L"0",text,32,config.c_str());
            wchar_t* end=nullptr;
            const long value=wcstol(text,&end,10);
            return count<31 && end!=text && !*end && value>=0 && value<=16384 ? value : -1L;
        };
        const auto x=readViewport(L"FGViewportX"), y=readViewport(L"FGViewportY");
        const auto w=readViewport(L"FGViewportW"), h=readViewport(L"FGViewportH");
        if(x<0||y<0||w<0||h<0||((w==0)!=(h==0))||(!w&&(x||y))) {
            Log("Invalid FGViewport override; skipping frame generation",GAKU_LOG_WARNING);
            frameGeneration=false;
        } else {
            FSR4_SetFgViewportOverride(x,y,w,h);
        }
    }
    if(frameGeneration) {
        float k1=0,k2=0,centerX=.5f,centerY=.5f;
        const auto readField=[&](const wchar_t* name,const wchar_t* fallback,float& result) {
            wchar_t text[64];
            const auto count=GetPrivateProfileStringW(L"FSR4",name,fallback,text,64,config.c_str());
            wchar_t* end=nullptr;
            result=wcstof(text,&end);
            return count<63 && end!=text && !*end && std::isfinite(result);
        };
        if(!readField(L"FGDistortionK1",L"0",k1) || !readField(L"FGDistortionK2",L"0",k2) ||
           !readField(L"FGDistortionCenterX",L"0.5",centerX) || !readField(L"FGDistortionCenterY",L"0.5",centerY) ||
           !FSR4_SetFgRadialDistortion(k1,k2,centerX,centerY)) {
            Log("Invalid FG radial distortion settings; using normal frame generation",GAKU_LOG_WARNING);
        }
    }
    if(frameGeneration&&!CaptureGameSwapChain(hooks))
        Log("DXGI factory hook unavailable; using a temporary swap chain for the Present hook",GAKU_LOG_WARNING);
    enabled=true;*state=nullptr;return GAKU_OK;
}
void Tick(P,float) {
    try {
        if(enabled&&!attempted) { attempted=true; Bind(); }
        char line[1024];for(int i=0;i<24&&FSR4_PollLog(line,sizeof line)>0;++i) Log(line);
        if(renderFailed.exchange(false)) Stop("FSR backend failed (see bridge log)");
        static bool reported=false;
        static bool fgReported=false;
        if(completed.load()&&(!reported||(frameGeneration&&!fgReported))) {
            Stats stats{};FSR4_GetStats(&stats);
            if(stats.frames&&!reported) { reported=true;Log(std::string("FSR active: ")+stats.provider); }
            if(stats.fgGenerated&&!fgReported) {
                fgReported=true;
                Log(std::string("FSR Frame Generation active: ")+stats.fgProvider);
            }
        }
        if(bound&&enabled&&submitted.load()>completed.load()+32) Stop("Render event is not being consumed");
    } catch(const std::exception& e) { Stop(e.what()); }
}
void Shutdown(P) {
    enabled=false;
    FSR4_StopFrameGenerationTimer();
    primaryCamera.Reset();
    // Drop managed roots while IL2CPP is alive; GPU resources remain valid for already queued packets.
    capture.camera.Clear();capture.motion.Clear();
    for(auto s:{&resources.color,&resources.depth,&resources.motion,&resources.output}) {
        s->handle.Clear();s->texture.Clear();
    }
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
