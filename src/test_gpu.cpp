#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include "TestHalfFloat.h"
#include "BridgeStats.h"
#include "FrameMath.h"
using Microsoft::WRL::ComPtr;
struct FrameData {
    void *color,*depth,*motion,*output;
    uint32_t renderW,renderH,displayW,displayH;
    float jitterX,jitterY,mvScaleX,mvScaleY,frameTimeMs,cameraNear,cameraFar,fovV,sharpness;
    int32_t reset;uint32_t createFlags,dispatchFlags;float exposure;uint32_t direct,maxRenderW,maxRenderH;
    void* hudless;uint32_t fg;float pos[3],up[3],right[3],fwd[3];
};
static_assert(sizeof(FrameData)==176);
void Check(HRESULT hr){ if(FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
bool CheckBgraRoundTrip(ID3D11Device* device, ID3D11DeviceContext* context);
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    puts("GPU test: starting standalone harness");
    if(argc!=3&&argc!=4) return 2;
    const bool nativeAA=argc==4&&wcscmp(argv[3],L"--native-aa")==0;
    const bool quality=argc==4&&wcscmp(argv[3],L"--quality")==0;
    const bool dynamic=argc==4&&wcscmp(argv[3],L"--dynamic")==0;
    const bool fgScaled=argc==4&&wcscmp(argv[3],L"--fg-scaled")==0;
    const bool fgLetterbox=argc==4&&wcscmp(argv[3],L"--fg-letterbox")==0;
    const bool fgOffset=argc==4&&wcscmp(argv[3],L"--fg-offset")==0;
    const bool fgCross=fgOffset||(argc==4&&wcscmp(argv[3],L"--fg-cross")==0);
    const bool fgResized=fgScaled||fgLetterbox||fgCross;
    const bool fgDistortion=argc==4&&wcscmp(argv[3],L"--fg-distortion")==0;
    const bool fgHudless=argc==4&&wcscmp(argv[3],L"--fg-hudless")==0;
    const bool fgBgra=argc==4&&wcscmp(argv[3],L"--fg-bgra")==0;
    const bool fgRgb10=argc==4&&wcscmp(argv[3],L"--fg-rgb10")==0;
    const bool unsupportedFormat=fgBgra||fgRgb10;
    const bool fg=unsupportedFormat||fgHudless||fgDistortion||fgResized||(argc==4&&wcscmp(argv[3],L"--fg")==0);
    const bool portrait=nativeAA||quality||fgCross||(argc==4&&wcscmp(argv[3],L"--portrait")==0);
    if(argc==4&&!portrait&&!fg&&!dynamic) return 2;
    auto dll=LoadLibraryW(argv[1]);if(!dll) return 3;
    puts("GPU test: bridge loaded");
    auto load=reinterpret_cast<int(*)(const wchar_t*)>(GetProcAddress(dll,"FSR4_Load"));
    auto event=reinterpret_cast<void*(*)()>(GetProcAddress(dll,"FSR4_GetRenderEventFunc"));
    auto stats=reinterpret_cast<void(*)(Stats*)>(GetProcAddress(dll,"FSR4_GetStats"));
    auto poll=reinterpret_cast<int(*)(char*,int)>(GetProcAddress(dll,"FSR4_PollLog"));
    if(!load||!event||!stats||!poll||!load(argv[2])) return 4;
    auto loadFg=reinterpret_cast<int(*)(const wchar_t*)>(GetProcAddress(dll,"FSR4_LoadFrameGeneration"));
    if(fg&&(!loadFg||!loadFg(argv[2]))) return 4;
    if(fgOffset) {
        auto setViewport=reinterpret_cast<void(*)(uint32_t,uint32_t,uint32_t,uint32_t)>(
            GetProcAddress(dll,"FSR4_SetFgViewportOverride"));
        if(!setViewport) return 4;
        setViewport(50,0,608,1080);
    }
    if(fgDistortion) {
        auto setDistortion=reinterpret_cast<int(*)(float,float,float,float)>(
            GetProcAddress(dll,"FSR4_SetFgRadialDistortion"));
        if(!setDistortion || !setDistortion(0.1f,0,0.5f,0.5f)) return 4;
    }
    auto hudlessView=reinterpret_cast<uint64_t(*)(void**)>(GetProcAddress(dll,"FSR4_GetHudlessView"));
    if(fgHudless && (!hudlessView || hudlessView(nullptr)!=0)) return 4;
    puts("GPU test: AMD runtime loaded");
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
        puts("GPU test: D3D11 device created");
        if(fgBgra && !CheckBgraRoundTrip(device.Get(),ctx.Get())) return 14;
        auto make=[&](int w,int h,DXGI_FORMAT format,UINT bind) {
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.Format=format;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.BindFlags=bind;
            ComPtr<ID3D11Texture2D> t;Check(device->CreateTexture2D(&d,nullptr,&t));return t;
        };
        const int dw=fgCross?1080:fgLetterbox?3840:(fgScaled||nativeAA||quality)?2160:fg?960:portrait?1080:1920;
        const int dh=fgCross?1920:fgLetterbox?1634:(fgScaled||nativeAA||quality)?3840:fg?540:portrait?1920:1080;
        int w=nativeAA?dw:dw*2/3,h=nativeAA?dh:dh*2/3;
        auto color=make(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        auto depth=make(w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        auto motion=make(w,h,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        auto output=make(dw,dh,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        std::vector<uint16_t> pixels(w*h*4);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            int p=(y*w+x)*4;pixels[p]=x<w/2?0x3400:0x3a00;pixels[p+1]=0x3800;pixels[p+2]=0x3000;pixels[p+3]=0x3c00;
        }
        std::vector<float> z(w*h,0.5f);std::vector<uint16_t> mv(w*h*2,0);
        ctx->UpdateSubresource(color.Get(),0,nullptr,pixels.data(),w*8,0);
        ctx->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
        ctx->UpdateSubresource(motion.Get(),0,nullptr,mv.data(),w*4,0);
        FrameData fd{};fd.color=color.Get();fd.depth=depth.Get();fd.motion=motion.Get();fd.output=output.Get();
        fd.renderW=fd.maxRenderW=w;fd.renderH=fd.maxRenderH=h;fd.displayW=dw;fd.displayH=dh;
        fd.mvScaleX=-w;fd.mvScaleY=-h;fd.frameTimeMs=16.67f;fd.cameraNear=0.1f;fd.cameraFar=1000;
        fd.fovV=1.0f;fd.sharpness=0.2f;fd.createFlags=(1<<3)|(1<<5)|((nativeAA||quality)?1:0);fd.exposure=1;
        auto render=reinterpret_cast<void(__stdcall*)(int,void*)>(event());
        HWND window=nullptr;
        ComPtr<IDXGISwapChain1> swapChain;
        if(fg) {
            window=CreateWindowExW(0,L"STATIC",L"FSR Frame Generation GPU test",WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,CW_USEDEFAULT,320,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!window) return 8;
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            Check(device.As(&dxgi));
            Check(dxgi->GetAdapter(&adapter));
            Check(adapter->GetParent(IID_PPV_ARGS(&factory)));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width=fgCross?1920:fgResized?dw/2:dw;
            desc.Height=fgCross?1080:fgLetterbox?1080:fgScaled?dh/2:dh;
            desc.Format=fgBgra?DXGI_FORMAT_B8G8R8A8_UNORM:fgRgb10?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if(fgLetterbox) desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            Check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&swapChain));
            ShowWindow(window,SW_SHOWNOACTIVATE);
            fd.fg=1;fd.frameTimeMs=50;
            fd.up[1]=fd.right[0]=fd.fwd[2]=1;
        }
        const int frameCount=fg?5:dynamic?8:4;
        ComPtr<ID3D11Texture2D> dynamicReadback;
        const float positions[][2]={{.22f,.25f},{.38f,.25f},{.38f,.48f},{.60f,.65f},
                                    {.60f,.40f},{.33f,.40f},{.33f,.21f},{.49f,.21f}};
        float oldX=0,oldY=0,oldFov=0;
        if(dynamic) {
            // HDR, automatic exposure and dynamic resolution with a fixed display target.
            fd.createFlags|=(1<<0)|(1<<6);
            fd.exposure=0;
            D3D11_TEXTURE2D_DESC desc{};
            output->GetDesc(&desc);
            desc.Usage=D3D11_USAGE_STAGING;
            desc.BindFlags=0;
            desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(device->CreateTexture2D(&desc,nullptr,&dynamicReadback));
        }
        UINT presentCalls[5]{};
        for(int i=0;i<frameCount;++i) {
            fd.reset=i==0||(fg&&i==frameCount-1);
            int objectX=0,objectY=0;
            float light=1;
            if(dynamic) {
                const bool resized=i==5;
                if(resized) {
                    w=static_cast<int>(fd.maxRenderW)*3/4;
                    h=static_cast<int>(fd.maxRenderH)*3/4;
                    color=make(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
                    depth=make(w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
                    motion=make(w,h,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
                    pixels.resize(static_cast<size_t>(w)*h*4);
                    z.assign(static_cast<size_t>(w)*h,0.5f);
                    mv.resize(static_cast<size_t>(w)*h*2);
                    fd.color=color.Get();fd.depth=depth.Get();fd.motion=motion.Get();
                    fd.renderW=w;fd.renderH=h;
                    ctx->UpdateSubresource(depth.Get(),0,nullptr,z.data(),w*4,0);
                }
                objectX=static_cast<int>(positions[i][0]*w);
                objectY=static_cast<int>(positions[i][1]*h);
                const float fov=i<3?50.0f:70.0f;
                const float position[3]{i<3?0.0f:100.0f,0,0};
                const float forward[3]{i<3?0.0f:1.0f,0,i<3?1.0f:0.0f};
                fd.reset=resized||frame_math::Reset(1,1,i,i-1,fov,oldFov,position,fd.pos)||
                    frame_math::RotationJump(forward,fd.fwd);
                if(fd.reset!=(i==0||i==3||resized)) return 12;
                memcpy(fd.pos,position,sizeof position);
                memcpy(fd.fwd,forward,sizeof forward);
                fd.fovV=fov*0.01745329252f;
                oldFov=fov;
                fd.jitterX=i%2?0.25f:-0.25f;
                fd.jitterY=(i%3-1)*0.25f;
                fd.mvScaleX=static_cast<float>(w);
                fd.mvScaleY=static_cast<float>(h);
                const auto mx=TestFloatToHalf(fd.reset?0:oldX-static_cast<float>(objectX)/w);
                const auto my=TestFloatToHalf(fd.reset?0:oldY-static_cast<float>(objectY)/h);
                light=i==6?0.30f:i==7?0.80f:1.0f;
                const auto bright=TestFloatToHalf(light);
                const auto dark=TestFloatToHalf(0.22f*light);
                const auto mid=TestFloatToHalf(0.65f*light);
                for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                    const bool moving=std::abs(x-objectX)<w/32 && std::abs(y-objectY)<h/24;
                    const size_t p=(static_cast<size_t>(y)*w+x)*4;
                    pixels[p]=moving?bright:x<w/2?dark:mid;
                    pixels[p+1]=pixels[p+2]=dark;
                    pixels[p+3]=0x3c00;
                    const size_t m=(static_cast<size_t>(y)*w+x)*2;
                    mv[m]=moving?mx:0;
                    mv[m+1]=moving?my:0;
                }
                ctx->UpdateSubresource(color.Get(),0,nullptr,pixels.data(),w*8,0);
                ctx->UpdateSubresource(motion.Get(),0,nullptr,mv.data(),w*4,0);
            }
            printf("GPU test: dispatch %d\n",i);
            if(fgHudless) fd.hudless=i%2==0 ? fd.output : nullptr;
            render(1,&fd);
            if(dynamic) {
                ctx->CopyResource(dynamicReadback.Get(),output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(ctx->Map(dynamicReadback.Get(),0,D3D11_MAP_READ,0,&mapped));
                const auto threshold=TestFloatToHalf(0.83f*light);
                uint64_t sumX=0,sumY=0,count=0;
                for(int y=0;y<dh;++y) {
                    auto* row=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData)+mapped.RowPitch*y);
                    for(int x=0;x<dw;++x) {
                        const auto red=row[x*4];
                        if(red>=threshold && red<0x7c00) { sumX+=x; sumY+=y; ++count; }
                    }
                }
                const auto* oldRow=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData)+
                    mapped.RowPitch*static_cast<int>(oldY*dh));
                const auto oldPixel=oldRow[static_cast<int>(oldX*dw)*4];
                ctx->Unmap(dynamicReadback.Get(),0);
                oldX=static_cast<float>(objectX)/w;
                oldY=static_cast<float>(objectY)/h;
                const double actualX=count?static_cast<double>(sumX)/count:0;
                const double actualY=count?static_cast<double>(sumY)/count:0;
                printf("dynamic %d %dx%d reset=%d light=%.2f center=(%.1f,%.1f) expected=(%.1f,%.1f) old-center=0x%04X threshold=0x%04X\n",
                    i,w,h,fd.reset,light,actualX,actualY,oldX*dw,oldY*dh,oldPixel,threshold);
                if(count<static_cast<uint64_t>(dw)*dh/400 ||
                   std::abs(actualX-oldX*dw)>8 || std::abs(actualY-oldY*dh)>8 ||
                   (i>0 && oldPixel>=threshold)) return 12;
            }
            if(fgHudless) {
                void* view=nullptr;
                const auto generation=hudlessView(&view);
                const bool expected=i%2==0;
                if((generation!=0)!=expected || (view!=nullptr)!=expected) return 10;
            }
            if(!fg) continue;
            ComPtr<ID3D11Texture2D> backbuffer;
            ComPtr<ID3D11RenderTargetView> target;
            Check(swapChain->GetBuffer(0,IID_PPV_ARGS(&backbuffer)));
            Check(device->CreateRenderTargetView(backbuffer.Get(),nullptr,&target));
            const float rgba[]{0.2f+i*0.05f,0.3f,0.4f,1};
            ctx->ClearRenderTargetView(target.Get(),rgba);
            Sleep(50);
            UINT before=0,after=0;
            Check(swapChain->GetLastPresentCount(&before));
            DXGI_PRESENT_PARAMETERS parameters{};
            const UINT sync=fgLetterbox?0:fgScaled?2:1;
            const UINT flags=fgLetterbox?DXGI_PRESENT_ALLOW_TEARING:0;
            Check(i%2==0?swapChain->Present(sync,flags):swapChain->Present1(sync,flags,&parameters));
            Check(swapChain->GetLastPresentCount(&after));
            presentCalls[i]=after-before;
        }
        if(unsupportedFormat) render(0,nullptr); // Publish the final fallback counters.
        bool reducedFgSize=false;
        bool letterboxViewport=false;
        bool crossViewport=false;
        bool synchronizedPair=false;
        bool distortionUploaded=false, distortionRejected=false, formatLogged=false;
        char log[1024];while(poll(log,sizeof log)>0) {
            puts(log);
            formatLogged|=strstr(log,fgBgra?"BGRA8 (requires a format-conversion path)":"RGB10A2")!=nullptr;
            distortionUploaded|=strstr(log,"FG distortion: uploaded radial UV offsets")!=nullptr;
            distortionRejected|=strstr(log,"FG provider rejected radial distortion field")!=nullptr;
            reducedFgSize|=strstr(log,"FG context: inputs up to 1440x2560, display 1440x2560")!=nullptr;
            letterboxViewport|=strstr(log,"FG centered viewport: 0,131 1920x817 in 1920x1080.")!=nullptr;
            crossViewport|=strstr(log,fgOffset ? "FG configured viewport: 50,0 608x1080 in 1920x1080." :
                "FG centered viewport: 656,0 608x1080 in 1920x1080.")!=nullptr;
            synchronizedPair|=strstr(log,"FG synchronizes flip-model frame pairs to vblank.")!=nullptr;
        }
        if(fgDistortion && (!distortionUploaded || distortionRejected)) return 10;
        Stats s{};stats(&s);
        printf("frames=%llu, provider=%s, result=%d, failed=%d\n",s.frames,s.provider,s.lastResult,s.failed);
        if(s.frames!=frameCount||s.failed||s.lastResult||strncmp(s.provider,"4.",2)) return 5;
        if(dynamic && s.contextStarts!=1) return 12;
        if(fg) {
            auto skipReason=reinterpret_cast<const char*(*)()>(GetProcAddress(dll,"FSR4_GetLastFgSkipReason"));
            const char* reason=skipReason?skipReason():nullptr;
            if(unsupportedFormat) {
                if(!reason || !strstr(reason,"Incompatible backbuffer") || !formatLogged ||
                   s.fgGenerated || s.fgReal || s.fgSkipped!=static_cast<uint64_t>(frameCount)) return 15;
                for(UINT count:presentCalls) if(count!=1) return 15;
                puts("PASS: unsupported format shows its skip reason and presents every real frame.");
            } else if(!reason || strcmp(reason,"History reset or first frame")) return 11;
            printf("FG provider=%s, prepared=%llu, generated=%llu, real=%llu, Presents/frame=%u,%u,%u,%u,%u\n",
                s.fgProvider,s.fgPrepared,s.fgGenerated,s.fgReal,
                presentCalls[0],presentCalls[1],presentCalls[2],presentCalls[3],presentCalls[4]);
            const uint64_t expectedGenerated=fgResized?2:3;
            if(!unsupportedFormat && (strncmp(s.fgProvider,"4.0.2",5)||s.fgPrepared!=frameCount||s.fgGenerated!=expectedGenerated||
               s.fgReal!=expectedGenerated||reducedFgSize!=fgScaled||letterboxViewport!=fgLetterbox||crossViewport!=fgCross||
               synchronizedPair!=fgLetterbox||presentCalls[0]!=1||
               presentCalls[1]!=(fgResized?1u:2u)||presentCalls[2]!=2||presentCalls[3]!=2||presentCalls[4]!=1)) return 9;
            swapChain.Reset();
            DestroyWindow(window);
        }
        D3D11_TEXTURE2D_DESC readDesc{};output->GetDesc(&readDesc);
        readDesc.Usage=D3D11_USAGE_STAGING;readDesc.BindFlags=0;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;Check(device->CreateTexture2D(&readDesc,nullptr,&read));
        ctx->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto row=reinterpret_cast<uint16_t*>(static_cast<char*>(mapped.pData)+mapped.RowPitch*(dh/2));
        uint16_t left=row[(dw/4)*4],right=row[(dw*3/4)*4];
        ctx->Unmap(read.Get(),0);
        printf("output half-float samples: left=0x%04X right=0x%04X\n",left,right);
        if(left==0||right<=left||right>=0x7c00) return 6;
        render(2,nullptr);
        if(fgHudless) {
            void* view=nullptr;
            if(hudlessView(&view) || view) return 10;
        }
        puts(unsupportedFormat?"PASS: FSR dispatch and safe real-frame fallback."
               :fg?"PASS: FSR FG 4.0.2 GPU dispatch and generated frames through Present and Present1."
               :dynamic?"PASS: moving object position and old-location ghost checks, camera cuts, dynamic resolution and auto exposure."
               :"PASS: actual FSR 4 GPU dispatch and output readback; the game was not started.");
        // Keep the module resident: its backend owns the D3D12 context until process exit.
        return 0;
    } catch(const std::exception& e) { puts(e.what());return 7; }
}
