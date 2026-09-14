# 引き継ぎ

## いまどこまで

`JW_CADV.EXE` が**文字を描くところまで**動きます。そのあと `CS=000F` の
低位メモリに飛んで、11 億命令ほどゴミを実行し続けます。

```sh
sh build.sh
./dosemu --root ../jwcad_dos_wasm/orig \
         --font-ank  ../jwcad_dos_wasm/font/JWANK16.FNT \
         --font-kanji ../jwcad_dos_wasm/font/JWKAN16.FNT \
         ../jwcad_dos_wasm/orig/JW_CADV.EXE
```

出るのは `ESC[2J` と `*` だけです。画面は VRAM に描かれていて、
それを取り出す道具がまだありません。

## 次にやること

1. **VGA のプレーンを持つ。** これが無いと画面が取り出せません。
   `A000:0000` への読み書きをグラフィックコントローラ越しにする必要があります。
   **移植側の [`src/vga.c`](https://github.com/yomei-o/jwcad_dos_wasm/blob/main/src/vga.c)
   がそのまま使えます**——4 プレーン、ラッチ、Set/Reset、ビットマスク、
   書き込みモード 0 / 2 / 3 が入っていて、JW_CAD が使うのはその 3 つだけです。
2. **`INT 33h` のマウス。** 付属ドキュメントが `MOUSE.COM` 必須と言っています。
   いまは無応答（`int21_default` が何もせず true を返す）なので、
   `AX=0` のリセットに「マウス無し」を返している格好になります。
   低位メモリへの暴走はこのあたりが怪しい。
3. **画面のダンプ。** VGA が入ったら、`--screenshot N.png` のような形で
   N 命令ごと・またはキー入力のたびに書き出せるようにする。
4. **WASM 版。** 本家 dos_emu_cpp と同じ形で。
5. **突き合わせ。** jwcad_dos_wasm の `tools/check.sh` が
   「ネイティブ対 WASM」で 1 画素ずつ比べているので、その相手をここに変える。

## 刺された罠

**BIOS データエリアを空のままにしない。** 中立ではありません。
JW_CAD は画面の横幅を `0040:004A`、文字セルの高さを `0040:0085` から取って
**割り算します**。0 のままだと `R6003 - integer divide by 0` で、
何も描かずに止まります。しかも Microsoft C のランタイムが出すメッセージなので、
`DIV` 命令を捕まえても引っかかりません（ランタイム側で検査して落としている）。

**`INT 10h` を「受けて無視」にしない。** `AH=0Fh` でモードを取って
終了時に戻すプログラムは多く、返り値が無いと変な値を覚えます。

**DOS/V のプログラムは字形を持っていない。** `INT 15h AX=5000h` で
「字形を写すルーチン」のアドレスを尋ね、それを far call します。
実装が無いと関数ポインタがゴミのままで、最初の 1 文字で落ちます。
これは **`cd 15` をバイト列で探しても見つかりません**——DOS/V のプログラムは
たいてい `int86()` 経由で呼ぶので、機械語に `INT` 命令が現れないからです。

**フォントは外から渡せるようにしておく。** 移植の検証に使うなら、
**両方がまったく同じ字形を描かなければ比較に意味がありません。**
だから `--font-ank` / `--font-kanji` で FONTX2 を渡す形にしてあります。

## 押さえた番地（JW_CAD 側）

ロード基底は `0x110` でした（DGROUP が `3485`、これは
[jwcad_dos_wasm](https://github.com/yomei-o/jwcad_dos_wasm) の
リンク時アドレス `3375` ＋ `0x110`）。トレースを読むときの換算に使います。

| エミュレータの CS | リンク時 | 中身 |
|---|---|---|
| `0110` | `0000` | `main` のある最初のコードセグメント |
| `11B9` | `10a9` | 描画（`20a9:03e1` が文字、`20a9:07dc` が線） |
| `0EFF` | `0def` | コンソール層（`1def:23c5` が文字列描画） |
| `2B85` | `2a75` | Shift-JIS / JIS 変換 |
| `23C2` | `22b2` | Microsoft C のランタイム |
| `3485` | `3375` | DGROUP |
| `2BC8` | `2ab8` | オーバーレイが載る穴 |

JW_CAD 側の構造・オーバーレイ・ファイル形式は
[jwcad_dos_wasm の RESUME.md](https://github.com/yomei-o/jwcad_dos_wasm/blob/main/RESUME.md)
に全部書いてあります。

## 使えるトレース

本家から引き継いだものです。

```sh
DOSEMU_DOS_TRACE=1      # INT 21h を 1 本ずつ
DOSEMU_TRACE=lo-hi      # 命令番号の範囲で 1 命令ずつ
DOSEMU_SAMPLE=n         # n 命令ごとに CS:IP
DOSEMU_WATCH=lo-hi      # 物理番地の読み書き
```

落ちる場所を追うときは `DOSEMU_SAMPLE` で当たりを付けて、
`DOSEMU_TRACE` で範囲を絞るのが速いです。
