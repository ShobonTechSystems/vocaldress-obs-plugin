# VocalDress の声（OBS プラグイン）

OBS Studio の音声ソース「VocalDress の声」です。歌枠用ボーカルアプリ VocalDress が整えた声を、OBS へ途切れずに、時刻をそろえて渡します。

An OBS Studio audio source that receives the processed voice from the VocalDress app over shared memory.

## ライセンス

- このプラグイン（`obs-plugin/`）：GPL-2.0-or-later（[LICENSE](LICENSE)）。OBS Studio の libobs にリンクするためです
- アプリとプラグインの受け渡しの約束（`shared/voice-link/`）：MIT（[shared/voice-link/LICENSE](../shared/voice-link/LICENSE)）。形は [PROTOCOL.md](../shared/voice-link/PROTOCOL.md) にあります
- VocalDress のアプリ本体は、このプラグインとは別のプログラムで、共有メモリで声を渡すだけです。アプリのソースは公開していません

## 対象

- Windows 11 x64 の OBS Studio 32.0 以上（予定）。OBS の大きな版が出たら、読み込めるかを確かめてから知らせます
- OBS の「セーフモード」では、他社のプラグインは読み込まれません

## 入れる場所

VocalDress の有料版のインストーラーが入れます。自分で入れ直すときは、DLL を次の2か所に置きます（OBS 33 からの新しい形と、32 までの形）。

```
C:\ProgramData\obs-studio\plugins\vocaldress-voice\vocaldress-voice.dll
C:\ProgramData\obs-studio\plugins\vocaldress-voice\bin\64bit\vocaldress-voice.dll
```

OBS を閉じてから置き、OBS を開いて、ソースの「＋」から「VocalDress の声」を足します。消すときは `vocaldress-voice` のフォルダを消します。

## ビルド（Windows）

Visual Studio（C++ のデスクトップ開発）、CMake、Git が要ります。「Developer PowerShell for VS」で、このフォルダ（`obs-plugin` の1つ上）から：

1. 対象の一番古い版の OBS のソースを取る（ヘッダーだけを使う。libobs は、自分より新しい版のヘッダーでビルドしたプラグインを読まないため）

   ```powershell
   git clone --depth 1 --branch 32.0.0 https://github.com/obsproject/obs-studio.git obs-src
   ```

2. インストールされている OBS の `obs.dll` から、リンク用の `obs.lib` を作る（OBS を別の場所に入れたら `-ObsDll` で指定）

   ```powershell
   powershell -ExecutionPolicy Bypass -File obs-plugin\tools\make-obs-lib.ps1
   ```

3. ビルドする

   ```powershell
   cmake -S obs-plugin -B build -DOBS_SOURCE_DIR="$PWD\obs-src" -DOBS_IMPORT_LIB="$PWD\obs-lib\obs.lib"
   cmake --build build --config Release
   ```

   `build\Release\vocaldress-voice.dll` ができます。

## ビルド（Linux、試験用）

Ubuntu では `libobs-dev` を入れて `cmake -S obs-plugin -B build && cmake --build build && ctest --test-dir build` で、プラグインと、libobs に読み込ませて声が届くかを見る試し（`vocaldress_voice_smoke`、`xvfb-run` があれば仮の画面で動く）ができます。Linux の OBS は対象ではありません。
