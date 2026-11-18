# GakuFSR4

学園アイドルマスターのWindows版向けFSR 4 MODです。[GakuLoader](https://github.com/Gaku-Mod-Tools/GakuLoader)のプラグインとして動作します。

既定のQualityモードでは、3Dを出力解像度の各辺約2/3で描き、FSR 4で拡大します。v0.1.8はRadeon RX 9070 XTでゲーム内動作を確認済みです（FSR 4.1.2、1440×2560 → 2160×3840）。フレーム生成は対象外です。

## ビルド

Windows x64、Visual Studio 2022 C++ Build Tools、Windows SDK、CMakeが必要です。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 導入

GakuLoader導入済みのゲームフォルダーへ以下のファイルを配置します。

| ファイル | 配置先 |
| --- | --- |
| `build/Release/GakuFSR4.dll` | `GakuLoader/plugins/` |
| `gakufsr4.ini` | `GakuLoader/Configs/` |
| `amd_fidelityfx_loader_dx12.dll` と `amd_fidelityfx_upscaler_dx12.dll` | `GakuLoader/PluginData/gakufsr4/` |

AMDのDLLは[公式FSR SDK v2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/signedbin)から取得してください。GakuLoaderとゲーム本体は別途必要です。

`gakufsr4.ini` の `Preset=Quality` を編集してプリセットを選べます。選択肢は `NativeAA`（1.0倍）、`Quality`（1.5倍）、`Balanced`（1.7倍）、`Performance`（2.0倍）、`UltraPerformance`（3.0倍）です。倍率は出力解像度と内部描画解像度の各辺の比です。

`Preset` が未指定なら、旧版の `Ratio` 設定が使われます。プリセット名が認識できない場合はQualityになります。`Enabled=0` で無効化できます。設定はゲーム再起動時に読み込まれます。ゲーム内での確認には `GakuLoader/Logs/gakuloader.log` の `FSR context` と `FSR active` を参照してください。

対象は対応GPUを使うDirect3D 11のゲームカメラです。FSR 4の処理には別のDirect3D 12デバイスを使用するので、性能は環境によって変わります。画質の比較や他の構成での長時間テストは未実施です。

## ライセンス

ソースはGPL-3.0です。第三者コンポーネントは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
