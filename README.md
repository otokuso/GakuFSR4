# GakuFSR4

[![Windows build and tests](https://github.com/otokuso/GakuFSR4/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/otokuso/GakuFSR4/actions/workflows/windows-ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

**『学園アイドルマスター』Windows版で AMD FidelityFX Super Resolution 4（FSR 4）と Frame Generation（FG）を試すための、[GakuLoader](https://github.com/Gaku-Mod-Tools/GakuLoader) 向け非公式プラグインです。**

ゲームの Direct3D 11 描画から必要な情報を取得し、別の Direct3D 12 デバイス上で FSR 4 のアップスケーリングを実行します。FGを有効にすると、条件が合う場面で実フレームの間に補間フレームを1枚挿入します。

> [!WARNING]
> **開発・検証中のMODです。** RX 9070 XTでの動作報告はありますが、ほかのGPUやドライバー、画面モードでの動作を保証するものではありません。ゲーム更新で使えなくなる場合があります。導入・使用は自己責任で行ってください。

## できること

| 機能 | 現在の状態 |
| --- | --- |
| **FSR 4 アップスケーリング** | Native AA / Quality / Balanced / Performance / Ultra Performance に対応。FSR 4.1.2で実機動作を確認 |
| **FSR Frame Generation** | 対応する3Dシーンで最大2倍相当の表示フレームを生成。FSR FG 4.0.2で実機動作を確認。初期設定ではOFF |
| **GakuLoader内の設定画面** | F1メニューからFSR、FG、画質プリセット、シャープネスを設定可能 |
| **動作状況・タイミング表示** | 実ゲームFPSとPresent FPS、FGのスキップ理由、処理時間、CPU側のフレーム間隔・入力タイミングなどを表示 |

画質やFPSの改善幅は、解像度・シーン・GPU負荷・画面の更新設定に左右されます。「FGをONにしたら常に2倍のFPSが出る」という意味ではありません。

## 動作環境と必要なもの

- 『学園アイドルマスター』Windows版と、導入済みの **[GakuLoader](https://github.com/Gaku-Mod-Tools/GakuLoader)**。
- AMD FSR 4に対応するRadeon GPU。現時点でのゲーム内検証環境は **Radeon RX 9070 XT / Windows 11** です。
- **FG 4.0.2には Windows 11 と Radeon RX 9000シリーズ以降が必要**です。
- **AMD FidelityFX SDK v2.4.0の署名付きランタイムDLL**。ゲーム本体やGakuLoaderには含まれていません。
- ソースからビルドする場合は、Windows x64、Visual Studio 2022 C++ Build Tools、Windows SDK、CMake。

> [!NOTE]
> 現在、GitHub Releasesにはビルド済み配布ファイルがありません。下記の手順でソースをビルドしてください。AMDのDLLはこのリポジトリで再配布していません。

## インストール

### 1. GakuFSR4をビルドする

PowerShellで実行します。

```powershell
git clone https://github.com/otokuso/GakuFSR4.git
cd GakuFSR4
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成功すると `build/Release/GakuFSR4.dll` が生成されます。`ctest` は主にロジック・プラグインの動作を確認するテストで、実ゲームの画質やGPU互換性までは検証しません。

### 2. AMD公式ランタイムを入手する

[AMD FidelityFX SDK v2.4.0 / signedbin](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/signedbin) から、次のファイルを取得します。

- `amd_fidelityfx_loader_dx12.dll`
- `amd_fidelityfx_upscaler_dx12.dll`
- `amd_fidelityfx_framegeneration_dx12.dll`（FGを使う場合のみ）

DLLは公式配布元から取得してください。第三者サイトのDLLセットを使う必要はありません。

### 3. ゲームフォルダーに配置する

以下の `ゲームフォルダー` は、**GakuLoaderが導入されているゲームのルートディレクトリ**を指します。

| ファイル | 配置先（ゲームフォルダーからの相対パス） |
| --- | --- |
| ビルドした `GakuFSR4.dll` | `GakuLoader/plugins/GakuFSR4.dll` |
| リポジトリの `gakufsr4.ini` | `GakuLoader/Configs/gakufsr4.ini` |
| AMDの `amd_fidelityfx_loader_dx12.dll` | `GakuLoader/PluginData/gakufsr4/` |
| AMDの `amd_fidelityfx_upscaler_dx12.dll` | `GakuLoader/PluginData/gakufsr4/` |
| AMDの `amd_fidelityfx_framegeneration_dx12.dll` | `GakuLoader/PluginData/gakufsr4/`（FG使用時） |

フォルダーがなければ作成します。配置ミスは付属スクリプトで確認できます（パスは自分の環境に置き換えてください）。

```powershell
.\scripts\VerifyRuntimeLayout.ps1 -GameRoot 'C:\path\to\game' -FrameGeneration
```

FGを使わない場合は `-FrameGeneration` を外します。この検査は**ファイルの有無だけ**を確認します。AMDのバージョンやGPUの対応状況は判定しません。

### 4. ゲームを起動し、動作を確認する

1. ゲームを起動し、3Dシーンを表示します。
2. **F1** でGakuLoaderを開き、**GakuFSR4** のパネルを選びます。
3. `Runtime diagnostics` の `FSR provider` や処理済みフレーム数を確認します。
4. `GakuLoader/Logs/gakuloader.log` で `FSR active` を検索します。FGが実際に動いた場合は `FSR Frame Generation active` も出力されます。

## 画質とフレーム生成の設定

F1メニューのGakuFSR4パネルでは、次の操作ができます。

| 項目 | 内容 |
| --- | --- |
| `Enable FSR 4` | FSR処理の有効・無効 |
| `Preset` | 内部描画解像度のプリセット |
| `Sharpness` | シャープネス（0～100%） |
| `Frame Generation (2x)` | FGを有効にする（対応シーンのみ） |
| `Native AA / jitter (experimental)` | Native AAのジッター周期を変更。既定値はFSR推奨の8周期 |
| `Save settings` | `gakufsr4.ini` へ設定を保存 |

**保存した設定は、ゲームの次回起動時に反映されます。** Native AAの履歴を手動リセットするボタンだけは、実行中に次のFSRフレームへ反映されます。その他の実験設定はINIを直接編集してください。

### 画質プリセット

| プリセット | 出力解像度 ÷ 内部解像度（各辺） | 内部描画の幅・高さ |
| --- | ---: | ---: |
| `NativeAA` | 1.0倍 | 100% |
| `Quality` **（既定）** | 1.5倍 | 約66.7% |
| `Balanced` | 1.7倍 | 約58.8% |
| `Performance` | 2.0倍 | 50% |
| `UltraPerformance` | 3.0倍 | 約33.3% |

例えば4K出力で`Quality`を選ぶと、3Dをおおむね **2560×1440** で描画し、FSR 4で3840×2160へ拡大します。UIやすべての画面要素が必ずこの内部解像度で描画されるわけではありません。

### INIの基本設定

`GakuLoader/Configs/gakufsr4.ini` の例です。

```ini
[FSR4]
Enabled=1
Preset=Quality
Sharpness=0.2
FrameGeneration=0
JitterPhaseMode=2
```

- `Enabled=0` でプラグインを無効化します。
- `Sharpness` は `0.0`～`1.0` で指定します（UIでは0～100%）。
- `FrameGeneration=1` でAMD Native SwapChainによるFGを有効にします。初期値は `0` です。旧設定の `NativeSwapChain` は無視されます。
- Native AAの画面カメラは、UnityのTAAパスでFSRを実行します。旧設定 `NativeAABeforePost` は無視されます。
- `JitterPhaseMode=2` **（新しい既定値）** はNative AAだけFSR推奨の8周期にし、Quality以上は従来のUnity 1024周期を維持します。`0` はすべてUnity 1024周期（旧設定）、`1` はすべてFSR推奨周期です。既存のINIに `JitterPhaseMode=0` が書かれている場合は自動変更されないため、8周期を使うには `2` に変更してください。
- `ProviderVersion=4.1.2` のように指定すると、使うFSR 4.xプロバイダーのバージョンを固定できます。指定版が見つからない場合はFSRを開始しません。省略時は利用可能な4.xのうち最新のバージョンを選びます。**FSR 3への自動切り替えはありません。**
- 旧設定 `Ratio` は `Preset` が未指定の場合だけ使われます。不明なプリセット名は `Quality` として扱われます。

ジッターやモーションベクトルの符号、FGの表示領域・歪み補正といった追加オプションは検証用です。根拠なく変更すると残像や表示崩れにつながります。詳しくは[技術資料](#技術資料と開発)を参照してください。

Native AAの画質を検証するときは、まずFGを無効にし、シャープネスを0に設定してジッター周期を比較してください。F1パネルのRuntime diagnosticsでは、処理位置、周期、最後のジッター値、FSR入力のHDRフラグ、キュー投入数と履歴リセット数を確認できます。詳細な比較手順は[Native AA検証ガイド](docs/NATIVE_AA_VALIDATION.md)にあります。

## Frame Generationの注意点

FGはAMD Native SwapChainを使います。起動時に対応条件を満たさない場合やSwapChainの作成に失敗した場合は、ゲームのSwapChainで実フレームを表示し、FSRアップスケーリングを続けます。DAFPやVRRの動作条件と実機での測定結果は、[AMD SwapChain検証記録](docs/NATIVE_FG_SWAPCHAIN.md)を参照してください。

**現時点でのFGの主な制約**

- FGはシングルサンプルの **RGBA8 SDR** のフリップモデルに対応します。ReShade AutoHDRなどでバックバッファがRGBA16F scRGBになる場合は、実フレームを表示します。
- FGの入力には最終的な合成映像を使うため、画面内のUIや字幕まで補間される場合があります。UIを完全に分離して後から合成する処理は未実装です。
- 黒帯のある画面では中央の映像領域を対象にし、黒帯などを実フレームから残します。中央以外の映像配置に対する自動Viewport検出は未実装です。
- 対応するシーンを取得できない場合や、FGのGPU処理が失敗した場合は、補間フレームをスキップします。
- 画面の切り替え、リサイズ、デバイス喪失、HDRの切り替えなどは引き続き検証が必要です。

見た目に問題がある場合は、まず `FrameGeneration=0` に戻してFSRアップスケーリング単独で比較してください。

## FPS・フレームペーシング・入力遅延を確認する

### F1画面で確認できる値

GakuLoaderのGakuFSR4パネルに、以下の診断情報が表示されます。

| 表示 | 意味 |
| --- | --- |
| `Game FPS` | 実ゲームフレームの処理頻度 |
| `Present FPS (incl. FG)` | FGを含むPresent呼び出しベースの頻度。モニターに表示された実FPSとは限りません |
| `Game Present gap` | 実ゲームフレームのPresent間隔（中央値 / p95 / p99、ms） |
| `Pacing jitter (stddev)` | 上記間隔の標準偏差。小さいほど間隔が揃っています |
| `Input event -> Present start` | 取得できたキーボード・マウスイベントから次の実ゲームPresent開始までの時間（中央値 / p95、ms） |
| `Runtime diagnostics` | FSR/FGプロバイダー、GPU処理時間、推定VRAM使用量、生成・スキップ数、直近のFGスキップ理由など |

タイミングの集計対象は**直近10秒間の実ゲームフレーム**で、画面内の集計値は約2秒ごとに更新されます。`n` は集計サンプル数です。入力イベントを取得できない環境では `--` になります。

ここでの入力値は、Windowsメッセージとして取得できた入力から**CPU側のPresent開始まで**の参考値です。入力デバイスから実際の画素が発光するまでの全遅延は表しません。FGで生成したフレームの画面表示間隔も、F1画面だけでは測れません。

### 画面に表示されたフレームを測る（PresentMon）

表示FPS、フレーム間隔のp95/p99、入力から表示までのソフトウェア推定遅延を比較するには、[Intel PresentMon 2.x](https://github.com/GameTechDev/PresentMon)で記録したCSVを使います。

1. 同じ3Dシーン・解像度・ドライバー・リフレッシュレートで、**FG OFF / AMD SwapChain FG** を別々に計測します。
2. PresentMonで対象ゲームのプロセスを指定してCSVへ記録します。FGの判別用に `--track_frame_type` を有効にします。
3. 次のスクリプトでCSVを比較します。`GAME.exe` とPresentMonのパスは環境に合わせて置き換えてください。

```powershell
.\PresentMon-2.x-x64.exe --process_name GAME.exe --track_frame_type --timed 45 --terminate_after_timed --output_file native-fg.csv
python scripts\analyze_presentmon.py --warmup 120 --require-input baseline.csv native-fg.csv
```

このスクリプトは表示フレーム間隔とPresentMon由来の入力遅延推定値を集計します。入力サンプルがないときは遅延を0msとして扱わず、`--require-input` 指定時はエラーになります。実際のクリックから画素応答までを厳密に測定したい場合は、別途光学計測が必要です。

採取条件・指標の定義・CSVの扱いは[ベンチマーク手順](docs/BENCHMARKING.md)を参照してください。

## うまく動かないとき

| 症状 | 確認すること |
| --- | --- |
| F1にGakuFSR4が出ない | `GakuFSR4.dll` の配置先、GakuLoaderの導入状況、ログを確認 |
| FSRが動作しない | `Enabled=1`、AMDのloader/upscaler DLL、対応GPU、`FSR active` / `AMD FSR DLL loading failed` 等のログを確認 |
| FGが動作しない | `FrameGeneration=1`、FG用DLL、Windows/GPU要件、バックバッファ形式、`Last FG skip` を確認。条件に合わなければ補間されません |
| FGで画面が乱れる・カクつく | `FrameGeneration=0` に設定して再起動し、FSR単独の動作と比較 |
| 設定を変えたのに反映されない | `Save settings` を押したか、正しいINIを編集したか確認し、**ゲームを再起動** |
| 入力遅延が `--` になる | 入力イベントを取得できないケースがあります。値がない状態であって、遅延がゼロという意味ではありません |

報告するときは、GPU/ドライバー、Windowsのバージョン、ゲームの画面モード、FSR/FG設定、再現手順、関連ログを[Issues](https://github.com/otokuso/GakuFSR4/issues)に添えてください。個人情報やゲーム本体のファイルは投稿しないでください。

## 技術資料と開発

このプロジェクトは、[GHPC-FSR4](https://github.com/marsattackk/GHPC-FSR4) のD3D11/D3D12ブリッジとAMD FidelityFX SDKのAPIを基にしています。ゲーム側のレンダリングフックやGakuLoader連携を追加しています。

| ドキュメント | 内容 |
| --- | --- |
| [GPU_TESTING.md](docs/GPU_TESTING.md) | GPU実機検証、検証用実行ファイル、CIで確認できない範囲 |
| [BENCHMARKING.md](docs/BENCHMARKING.md) | PresentMonによるFPS・ペーシング・入力タイミングの計測 |
| [NATIVE_FG_SWAPCHAIN.md](docs/NATIVE_FG_SWAPCHAIN.md) | AMD公式SwapChain経路と実機検証記録 |
| [FRAME_PACING.md](docs/FRAME_PACING.md) | 旧手動Present方式の計測記録 |
| [FG_FORMAT_SUPPORT.md](docs/FG_FORMAT_SUPPORT.md) | FGのバックバッファ形式とHDR制約 |
| [FG_UI_COMPOSITION.md](docs/FG_UI_COMPOSITION.md) | UI分離・合成に関する現状と未実装部分 |
| [FG_DISTORTION.md](docs/FG_DISTORTION.md) | FG用の実験的な歪みフィールド |
| [FSR_BEFORE_POST.md](docs/FSR_BEFORE_POST.md) | NativeAAをポストプロセス前に配置する実験設定 |
| [NATIVE_AA_VALIDATION.md](docs/NATIVE_AA_VALIDATION.md) | Native AAの処理位置・ジッター・HDR・モーションベクトルの検証方法 |
| [MIP_BIAS.md](docs/MIP_BIAS.md) | テクスチャMipバイアスの調査 |

Windows CIはDebug/ReleaseのビルドとCPU側テストを実行します。ゲーム内GPU動作の検証は別途必要です。手動GPUテストのコマンドは[GPU_TESTING.md](docs/GPU_TESTING.md)にあります。

## ライセンスとクレジット

GakuFSR4のソースコードは **[GPL-3.0](LICENSE)** で公開しています。AMDのランタイムDLLはAMDのライセンスに従う別配布物です。

GHPC-FSR4、AMD FidelityFX SDK、GakuLoaderのABIヘッダー、Dear ImGuiなどの出典・ライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を確認してください。

本プロジェクトは非公式のMODであり、ゲームの運営元やAMDによる承認・サポートを意味しません。
