#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
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
struct Stats { uint64_t frames,providerId;int32_t lastResult,failed;char provider[64],adapter[128];char rest[464]; };
static_assert(sizeof(Stats)==680);
void Check(HRESULT hr){ if(FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    puts("GPU test: starting standalone harness");
    if(argc!=3&&argc!=4) return 2;
    const bool nativeAA=argc==4&&wcscmp(argv[3],L"--native-aa")==0;
    const bool quality=argc==4&&wcscmp(argv[3],L"--quality")==0;
    const bool portrait=nativeAA||quality||(argc==4&&wcscmp(argv[3],L"--portrait")==0);
    if(argc==4&&!portrait) return 2;
    auto dll=LoadLibraryW(argv[1]);if(!dll) return 3;
    puts("GPU test: bridge loaded");
    auto load=reinterpret_cast<int(*)(const wchar_t*)>(GetProcAddress(dll,"FSR4_Load"));
    auto event=reinterpret_cast<void*(*)()>(GetProcAddress(dll,"FSR4_GetRenderEventFunc"));
    auto stats=reinterpret_cast<void(*)(Stats*)>(GetProcAddress(dll,"FSR4_GetStats"));
    auto poll=reinterpret_cast<int(*)(char*,int)>(GetProcAddress(dll,"FSR4_PollLog"));
    if(!load||!event||!stats||!poll||!load(argv[2])) return 4;
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
        const int dw=(nativeAA||quality)?2160:portrait?1080:1920;
        const int dh=(nativeAA||quality)?3840:portrait?1920:1080;
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
        for(int i=0;i<4;++i) { fd.reset=i==0;printf("GPU test: dispatch %d\n",i);render(1,&fd); }
        char log[1024];while(poll(log,sizeof log)>0) puts(log);
        Stats s{};stats(&s);
        printf("frames=%llu, provider=%s, result=%d, failed=%d\n",s.frames,s.provider,s.lastResult,s.failed);
        if(s.frames!=4||s.failed||s.lastResult||strncmp(s.provider,"4.",2)) return 5;
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
        puts("PASS: actual FSR 4 GPU dispatch and output readback; the game was not started.");
        // Keep the module resident: its backend owns the D3D12 context until process exit.
        return 0;
    } catch(const std::exception& e) { puts(e.what());return 7; }
}
