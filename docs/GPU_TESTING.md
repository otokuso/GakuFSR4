# Windows / GPU verification

The GitHub Actions `windows-ci.yml` workflow builds Debug and Release on Windows x64 / MSVC and runs `ctest`. `plugin_abi_and_ui` loads the produced plugin, checks the plugin descriptor ABI, draws the settings panel, and verifies startup without the game runtime. It intentionally does not require an AMD GPU.

## Manual GPU test (supported Radeon + Windows 11)

1. Build the Release configuration according to the README.
2. Obtain the signed AMD FidelityFX SDK v2.4.0 runtime DLLs from the official AMD SDK and put the loader, upscaler, and (for FG) frame-generation DLLs in a dedicated runtime directory. Do not commit or download binaries from unofficial mirrors.
3. Run `test_gpu.exe` outside CI, with the bridge DLL path and the runtime directory as its first two arguments. For example:

   ```powershell
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-scaled
   .\build\Release\test_gpu.exe .\build\Release\GakuFSR4.dll C:\AMD-FSR-runtime --fg-letterbox
   ```

4. In a game installation with GakuLoader, check deployment file paths:

   ```powershell
   .\scripts\VerifyRuntimeLayout.ps1 -GameRoot 'C:\path\to\game' -FrameGeneration
   ```

5. Record GPU model/driver, OS build, adapter/display resolution, FG enabled/disabled, game-camera orientation, and GPU test logs. Verify Present/Present1, portrait/landscape, resize, UI/HDR and device-loss cases separately in the real game. These cases are not covered by hosted Windows CI.

The deployment verifier checks file presence and names only; runtime versions and GPU compatibility still require real execution.
