# GakuFSR4

学園アイドルマスターのWindows版向けFSR 4 MODです。[GakuLoader](https://github.com/Gaku-Mod-Tools/GakuLoader)のプラグインとして動作します。

既定のQualityモードでは、3Dを出力解像度の各辺約2/3で描き、FSR 4.1.2で拡大します。FSR Frame Generation 4.0.2を有効にすると、対応する3Dシーンで実フレームの間に補間フレームを1枚表示します。

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
| `amd_fidelityfx_framegeneration_dx12.dll`（フレーム生成を使う場合） | `GakuLoader/PluginData/gakufsr4/` |

AMDのDLLは[公式FSR SDK v2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/signedbin)から取得してください。GakuLoaderとゲーム本体は別途必要です。

`gakufsr4.ini` の `Preset=Quality` を編集してプリセットを選べます。選択肢は `NativeAA`（1.0倍）、`Quality`（1.5倍）、`Balanced`（1.7倍）、`Performance`（2.0倍）、`UltraPerformance`（3.0倍）です。倍率は出力解像度と内部描画解像度の各辺の比です。

`Preset` が未指定なら、旧版の `Ratio` 設定が使われます。プリセット名が認識できない場合はQualityになります。`Enabled=0` で無効化できます。設定はゲーム再起動時に読み込まれます。ゲーム内での確認には `GakuLoader/Logs/gakuloader.log` の `FSR context` と `FSR active` を参照してください。

FSR 4.xのプロバイダーが複数ある場合は、利用可能な中でバージョン番号が最も新しいものを選びます。特定バージョンで検証したい場合は、INIの `[FSR4]` に `ProviderVersion=4.1.2` のように指定してください。指定版がなければFSRを開始せず、ログで理由を確認できます。省略すると自動選択に戻ります（FSR 3への自動フォールバックはしません）。

モーションベクトルの上下方向の検証用に `MotionVectorYSign=-1`（既定）または `MotionVectorYSign=1` を設定できます。Unity/URPの実際の符号が未検証のため、既定の符号は変更していません。上下移動する被写体を同じ条件で比較し、残像や輪郭の揺れを確認してください。

ジッターの座標系を比較する場合は `JitterXSign` / `JitterYSign` にそれぞれ `-1`（既定）か `1` を設定できます。変更時は射影行列・画面上のサブピクセル移動・FSRに渡す値が一致するかを同一フレームで検証してください。現時点で正しい符号は実機確認できておらず、既定の符号は変更していません。

画質比較専用の実験設定として `JitterPhaseMode=1` を指定すると、FSR倍率に合わせてジッター周期を変えます（Quality 18、Balanced 23、Performance 32、UltraPerformance 72）。既定の `0` では Unity の周期を使用します。

対象は対応GPUを使うDirect3D 11のゲームカメラです。FSR 4の処理には別のDirect3D 12デバイスを使用するので、性能は環境によって変わります。画質の比較や他の構成での長時間テストは未実施です。

フレーム生成は既定で無効です。`FrameGeneration=1` で有効になり、ゲームログの `FSR Frame Generation active` に実際に使ったバージョンが出ます。4.0.2にはWindows 11とRadeon RX 9000シリーズ以降が必要です。縦画面と横画面のSDR表示で、補間フレームを挟める表示間隔があるときに動作します。中央に表示される映像に黒帯が付く場合は、映像部分を補間して黒帯やその上のUIを実フレームから残します。映像部分のUIも補間の対象になります。ウィンドウが小さいときは、カメラの描画解像度を下限に補間の処理解像度を抑えます。

画面の映像領域が中央以外にある場合の実験用に、`FGViewportX`、`FGViewportY`、`FGViewportW`、`FGViewportH` でバックバッファ内の領域をピクセル単位で指定できます。指定した矩形がバックバッファ外またはシーンのアスペクト比と合わなければFGをスキップします。未指定時は従来の中央アスペクトフィットを使います。実Viewportの自動検出は未実装です。

RX 9070 XTで、ゲーム内の縦画面に対してFSR 4.1.2とFSR Frame Generation 4.0.2の動作を確認しました。

ゲームが垂直同期なしで動作している場合も、FGで補間フレームと実フレームを続けて表示するときは、各フレームをディスプレイの更新に同期させます。

## GakuLoaderで設定

ゲーム起動後にF1でGakuLoaderを開き、`GakuFSR4` パネルを選びます。FSR 4の有効・無効、フレーム生成、プリセット、シャープネス（0〜100%）を変更し、`Save settings` を押してください。同じINIファイルに保存され、ゲームの次回起動時に反映されます。

## ライセンス

ソースはGPL-3.0です。第三者コンポーネントは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
