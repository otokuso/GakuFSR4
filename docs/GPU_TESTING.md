# Windows / GPU verification

The GitHub Actions `windows-ci.yml` workflow builds Debug and Release on Windows x64 / MSVC and runs `ctest`. `plugin_abi_and_ui` loads the produced plugin, checks the plugin descriptor ABI, draws the settings panel, and verifies startup without the game runtime. It intentionally does not require an AMD GPU.

## Manual GPU test (supported Radeon + Windows 11)

1. Build the Release configuration according to the README.
2. Obtain the signed AMD FidelityFX SDK v2.4.0 runtime DLLs from the official AMD SDK and put the loader, upscaler, and (for FG) frame-generation DLLs in a dedicated runtime directory. Do not commit or download binaries from unofficial mirrors.
3. Run `test_gpu.exe` outside CI, with the bridge DLL path and the runtime directory as its first two arguments. For example:

   ```powershell
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --dynamic
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-scaled
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-letterbox
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-offset
   ```

   The dynamic GPU test logs each frame's render size, history reset, modeled lighting, detected object center, and sample at the object's previous position. It returns a nonzero exit code for large positioning errors, a bright ghost at that position, or a context recreation during the render-size change.

4. In a game installation with GakuLoader, check deployment file paths:

   ```powershell
   .\scripts\VerifyRuntimeLayout.ps1 -GameRoot 'C:\path\to\game' -FrameGeneration
   ```

5. Record GPU model/driver, OS build, adapter/display resolution, FG enabled/disabled, game-camera orientation, and GPU test logs. Verify Present/Present1, portrait/landscape, resize, UI/HDR and device-loss cases separately in the real game. These cases are not covered by hosted Windows CI.

6. When the installed FG provider reports a Configure, Prepare, or Dispatch failure, confirm that the plugin continues upscaling, stops attempting FG until each retry delay expires (0.5, 1, 2, 4, and 8 seconds), and stops retrying after the sixth consecutive error. The last skip reason should identify an exhausted retry budget. A successful FG dispatch should reset that budget; a scene or backbuffer resize should allow a new attempt. The host's CPU/GPU timing counters must continue to identify upscale work separately from FG work after a device reset. This is a manual fault-recovery check, not something the standard synthetic GPU harness injects today.

The deployment verifier checks file presence and names only; runtime versions and GPU compatibility still require real execution.
