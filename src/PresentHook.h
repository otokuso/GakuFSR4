#pragma once

#include <cstdint>

struct GakuHookService;
struct ID3D11Resource;

bool CaptureGameSwapChain(const GakuHookService& hooks, bool enableFrameGeneration);
void InstallPresentHook(ID3D11Resource* anchor);
bool PresentHooked();
uint64_t PresentsSeen();
void BridgeLog(const char* fmt, ...);
