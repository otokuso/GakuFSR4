# Windows / GPU verification

The GitHub Actions `windows-ci.yml` workflow builds Debug and Release on Windows x64 / MSVC and runs `ctest`. `plugin_abi_and_ui` loads the produced plugin, checks the plugin descriptor ABI, draws the settings panel, and verifies startup without the game runtime. It intentionally does not require an AMD GPU.

## Manual GPU test (supported Radeon + Windows 11)

1. Build the Release configuration according to the README.
2. Obtain the signed AMD FidelityFX SDK v2.4.0 runtime DLLs from the official AMD SDK and put the loader, upscaler, and (for FG) frame-generation DLLs in a dedicated runtime directory. Do not commit or download binaries from unofficial mirrors.
3. Run `test_gpu.exe` outside CI, with the bridge DLL path and the runtime directory as its first two arguments. For example:

   ```powershell
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --dynamic
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-fallback
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-bgra
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-hudless
   .\build\Release\test_native_swapchain.exe C:\AMD-FSR-runtime --fg
   ```

   The dynamic GPU test logs each frame's render size, history reset, modeled lighting, detected object center, and sample at the object's previous position. It returns a nonzero exit code for large positioning errors, a bright ghost at that position, or a context recreation during the render-size change. The fallback test requests FG on an ordinary D3D11 swap chain and verifies upscaling, zero FG preparation and one real frame per Present. The native harness exercises AMD interpolation in a standalone window.

4. In a game installation with GakuLoader, check deployment file paths:

   ```powershell
   .\scripts\VerifyRuntimeLayout.ps1 -GameRoot 'C:\path\to\game' -FrameGeneration
   ```

5. Record GPU model/driver, OS build, adapter/display resolution, FG enabled/disabled, game-camera orientation, and GPU test logs. Verify Present/Present1, portrait/landscape, resize, UI/HDR and device-loss cases separately in the real game. These cases are not covered by hosted Windows CI.

6. Test FG startup fallback and recovery with `FrameGeneration=1`. Verify that an unsupported swap-chain format or startup failure keeps real frames visible with FSR upscaling active. Exercise provider failures during Configure, Prepare and native SwapChain interpolation. FG retries should be delayed by 0.5, 1, 2, 4 and 8 seconds and stop after six consecutive failures. A successful interpolation resets the budget; a scene or backbuffer resize allows a new attempt. Check the runtime panel's last skip reason and the bridge log. The hosted tests exercise the retry policy, **not** fault injection into the AMD DLL.

7. On a system where the temporary DXGI swap-chain probe fails during startup, verify that a later probe can install the Present hook. Failed probes are limited to one attempt every five seconds. Check that the game Present timing panel recovers once the hook is installed.

The deployment verifier checks file presence and names only; runtime versions and GPU compatibility still require real execution.
