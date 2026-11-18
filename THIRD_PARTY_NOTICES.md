# Third-party components

## GHPC-FSR4

- https://github.com/marsattackk/GHPC-FSR4
- Commit: bc79afa6a57c8970c8b760108d440e4ff30b78e4
- GPL-3.0; full license in LICENSE.
- The D3D11/D3D12 bridge and AMD API headers are vendored under vendor/ghpc.
- Changes to bridge.cpp: require a numeric 4.x upscaling provider, load the frame-generation DLL on request, validate the D3D11 presentation path, and keep backend state process-resident to avoid COM destruction under the DLL loader lock.
- Changes to present.cpp: capture Unity's swap chain and route both Present and Present1 through manual frame generation when enabled.

The GakuFSR4 integration source is distributed under GPL-3.0 as well. AMD runtime binaries are separate, unmodified components under AMD's terms, not GPL-licensed binaries.

## AMD FSR SDK 2.4.0

- https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0
- Runtime binaries: Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll and amd_fidelityfx_framegeneration_dx12.dll.
- AMD's complete license and notices are in runtime/AMD-FSR-LICENSE.md (installed next to these binaries).
- AMD API headers bundled by GHPC-FSR4 retain their original notices. Refer to the AMD license for individual source-file exceptions.

## GakuLoader plugin ABI header

- include/GakuPlugin.h was obtained from the GakuMotion source snapshot 61635d6, which includes the GakuLoader ABI v1 header.
- https://github.com/crested-ezios/GakuMotion
- MIT license: include/GakuMotion-MIT-LICENSE.txt.
- Loader project: https://github.com/Gaku-Mod-Tools/GakuLoader

## Dear ImGui

- https://github.com/ocornut/imgui
- Version 1.89.4, from the same GakuMotion snapshot as the ABI header.
- MIT license: vendor/imgui/LICENSE.txt.

## Unity rendering references

The integration was written against the target game's metadata and informed by Unity's URP 2022.3/staging rendering source. Unity source files are not compiled into this plugin.

- https://github.com/Unity-Technologies/Graphics/tree/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime
- Target game UnityPlayer version inspected locally: 6000.0.77f1. Actual field offsets and method pointers are resolved at runtime; no addresses from another Unity version are used.
