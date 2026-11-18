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
using Microsoft::WRL::ComPtr;
struct FrameData {
    void *color,*depth,*motion,*output;
    uint32_t renderW,renderH,displayW,displayH;
    float jitterX,jitterY,mvScaleX,mvScaleY,frameTimeMs,cameraNear,cameraFar,fovV,sharpness;
    int32_t reset;uint32_t createFlags,dispatchFlags;float exposure;uint32_t direct,maxRenderW,maxRenderH;
    void* hudless;uint32_t fg;float pos[3],up[3],right[3],fwd[3];
};
static_assert(sizeof(FrameData)==176);
struct Stats {
    uint64_t frames,providerId;
    int32_t lastResult,failed;
    char provider[64],adapter[128],upscaleStats[304];
    char fgProvider[64];
    uint64_t fgProviderId,fgPrepared,fgGenerated,fgReal;
    char remainingStats[64];
};
static_assert(sizeof(Stats)==680);
void Check(HRESULT hr){ if(FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    puts("GPU test: starting standalone harness");
    if(argc!=3&&argc!=4) return 2;
    const bool nativeAA=argc==4&&wcscmp(argv[3],L"--native-aa")==0;
    const bool quality=argc==4&&wcscmp(argv[3],L"--quality")==0;
    const bool fgScaled=argc==4&&wcscmp(argv[3],L"--fg-scaled")==0;
    const bool fg=fgScaled||(argc==4&&wcscmp(argv[3],L"--fg")==0);
    const bool portrait=nativeAA||quality||(argc==4&&wcscmp(argv[3],L"--portrait")==0);
    if(argc==4&&!portrait&&!fg) return 2;
    auto dll=LoadLibraryW(argv[1]);if(!dll) return 3;
    puts("GPU test: bridge loaded");
    auto load=reinterpret_cast<int(*)(const wchar_t*)>(GetProcAddress(dll,"FSR4_Load"));
    auto event=reinterpret_cast<void*(*)()>(GetProcAddress(dll,"FSR4_GetRenderEventFunc"));
    auto stats=reinterpret_cast<void(*)(Stats*)>(GetProcAddress(dll,"FSR4_GetStats"));
    auto poll=reinterpret_cast<int(*)(char*,int)>(GetProcAddress(dll,"FSR4_PollLog"));
    if(!load||!event||!stats||!poll||!load(argv[2])) return 4;
    auto loadFg=reinterpret_cast<int(*)(const wchar_t*)>(GetProcAddress(dll,"FSR4_LoadFrameGeneration"));
    if(fg&&(!loadFg||!loadFg(argv[2]))) return 4;
    puts("GPU test: AMD runtime loaded");
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
        puts("GPU test: D3D11 device created");
        auto make=[&](int w,int h,DXGI_FORMAT format,UINT bind) {
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.Format=format;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.BindFlags=bind;
            ComPtr<ID3D11Texture2D> t;Check(device->CreateTexture2D(&d,nullptr,&t));return t;
        };
        const int dw=(fgScaled||nativeAA||quality)?2160:fg?540:portrait?1080:1920;
        const int dh=(fgScaled||nativeAA||quality)?3840:fg?960:portrait?1920:1080;
        const int w=nativeAA?dw:dw*2/3,h=nativeAA?dh:dh*2/3;
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
            desc.Width=fgScaled?dw/2:dw;desc.Height=fgScaled?dh/2:dh;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
            Check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&swapChain));
            ShowWindow(window,SW_SHOWNOACTIVATE);
            fd.fg=1;fd.frameTimeMs=50;
            fd.up[1]=fd.right[0]=fd.fwd[2]=1;
        }
        const int frameCount=fg?5:4;
        UINT presentCalls[5]{};
        for(int i=0;i<frameCount;++i) {
            fd.reset=i==0||(fg&&i==frameCount-1);
            printf("GPU test: dispatch %d\n",i);
            render(1,&fd);
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
            const UINT sync=fgScaled?2:1;
            Check(i%2==0?swapChain->Present(sync,0):swapChain->Present1(sync,0,&parameters));
            Check(swapChain->GetLastPresentCount(&after));
            presentCalls[i]=after-before;
        }
        bool reducedFgSize=false;
        char log[1024];while(poll(log,sizeof log)>0) {
            puts(log);
            reducedFgSize|=strstr(log,"FG context: inputs up to 1440x2560, display 1440x2560")!=nullptr;
        }
        Stats s{};stats(&s);
        printf("frames=%llu, provider=%s, result=%d, failed=%d\n",s.frames,s.provider,s.lastResult,s.failed);
        if(s.frames!=frameCount||s.failed||s.lastResult||strncmp(s.provider,"4.",2)) return 5;
        if(fg) {
            printf("FG provider=%s, prepared=%llu, generated=%llu, real=%llu, Presents/frame=%u,%u,%u,%u,%u\n",
                s.fgProvider,s.fgPrepared,s.fgGenerated,s.fgReal,
                presentCalls[0],presentCalls[1],presentCalls[2],presentCalls[3],presentCalls[4]);
            const uint64_t expectedGenerated=fgScaled?2:3;
            if(strncmp(s.fgProvider,"4.0.2",5)||s.fgPrepared!=frameCount||s.fgGenerated!=expectedGenerated||
               s.fgReal!=expectedGenerated||reducedFgSize!=fgScaled||presentCalls[0]!=1||
               presentCalls[1]!=(fgScaled?1u:2u)||presentCalls[2]!=2||presentCalls[3]!=2||presentCalls[4]!=1) return 9;
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
        puts(fg?"PASS: FSR FG 4.0.2 GPU dispatch and generated frames through Present and Present1."
               :"PASS: actual FSR 4 GPU dispatch and output readback; the game was not started.");
        // Keep the module resident: its backend owns the D3D12 context until process exit.
        return 0;
    } catch(const std::exception& e) { puts(e.what());return 7; }
}
