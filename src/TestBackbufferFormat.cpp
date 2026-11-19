#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
void Check(HRESULT hr);

bool CheckBgraRoundTrip(ID3D11Device* device, ID3D11DeviceContext* context)
{
    constexpr char shader[] = R"(
Texture2D source : register(t0);
float4 VS(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1, 0, 1);
}
float4 PS() : SV_Target { return source.Load(int3(0,0,0)); }
)";
    ComPtr<ID3DBlob> vertexCode, pixelCode;
    Check(D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr,
        "VS", "vs_5_0", 0, 0, &vertexCode, nullptr));
    Check(D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr,
        "PS", "ps_5_0", 0, 0, &pixelCode, nullptr));
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    Check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertex));
    Check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixel));
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    const D3D11_VIEWPORT viewport{0,0,1,1,0,1};
    context->RSSetViewports(1, &viewport);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const uint8_t bgra[]{0xc4,0x7f,0x19,0xe8}, rgba[]{0x19,0x7f,0xc4,0xe8};
    const D3D11_SUBRESOURCE_DATA input{bgra,sizeof bgra,0};
    ComPtr<ID3D11Texture2D> source, converted, roundTrip;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Check(device->CreateTexture2D(&desc, &input, &source));
    Check(device->CreateTexture2D(&desc, nullptr, &roundTrip));
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(device->CreateTexture2D(&desc, nullptr, &converted));

    const auto blit = [&](ID3D11Texture2D* from, ID3D11Texture2D* to) {
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateShaderResourceView(from, nullptr, &srv));
        Check(device->CreateRenderTargetView(to, nullptr, &rtv));
        auto* src=srv.Get();
        auto* dst=rtv.Get();
        context->PSSetShaderResources(0, 1, &src);
        context->OMSetRenderTargets(1, &dst, nullptr);
        context->Draw(3, 0);
        src=nullptr;
        context->PSSetShaderResources(0, 1, &src);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    };
    blit(source.Get(), converted.Get());
    blit(converted.Get(), roundTrip.Get());
    context->ClearState();

    const auto matches = [&](ID3D11Texture2D* texture, const uint8_t* expected) {
        texture->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;
        desc.BindFlags=0;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        Check(device->CreateTexture2D(&desc, nullptr, &readback));
        context->CopyResource(readback.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const bool equal=std::memcmp(mapped.pData, expected, 4)==0;
        context->Unmap(readback.Get(), 0);
        return equal;
    };
    if (!matches(converted.Get(), rgba) || !matches(roundTrip.Get(), bgra)) return false;
    puts("PASS: D3D11 shader BGRA8 -> RGBA8 -> BGRA8 preserves all four channels.");
    return true;
}
