# テストコマンド一覧

CLAUDE.md から切り出した、実機テストとホスト側テストのコマンド。**何を守るかの規則（押下回数の契約、ログマーカー、ホスト検査のパス直書きなど）は CLAUDE.md に残してある。** コマンドを足したらここに1行足す。

## 実機テスト

すべてUSBシリアル経由。ESP-IDFのPython環境（pyserial）で走らせる。

```powershell
python tools\smoke_device.py --port COM3 --cycles 20   # 起動/停止のライフサイクルとリーク
python tools\test_settings.py --port COM3              # XMB設定・ミュート順序・画面遷移
python tools\capture_home.py --port COM3               # 実ピクセル取得と30fps確認
python tools\test_editor_draft.py --port COM3          # 未保存の編集がアプリ起動を跨いで残るか
python tools\benchmark_app.py --port COM3              # JSアプリのPAINT内訳
python tools\stress_app.py --port COM3                # STRESS TEST（メニュー最後の行）: ヒープ負荷3段階＋描画負荷、OOM回復とfps
python tools\test_app_resume.py --port COM3           # Backで眠るアプリ（IMU CAL/PET/COMPANION）: 中断→同じ行で再開→別アプリで退去
```

## ホスト側のテスト（実機不要）

```bash
python tools/test_flash_budget.py       # パーティション予約ガード
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && gcc -O2 -Wall -Wextra -Werror tools/test_solar_sail.c -lm -o /tmp/ts && /tmp/ts"
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && gcc -O2 -Wall -Wextra -Werror tools/test_flower.c main/scene/canopy_pie.c main/scene/garden_decor_pie.c -I main/scene -I tools/hostshim -lm -o /tmp/tf && /tmp/tf"   # カーネル2ファイルも一緒にリンクする（flower.c単体では未定義参照）
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && gcc -O2 -Wall -Wextra -Werror tools/test_solar_time.c -lm -o /tmp/t && /tmp/t"   # WSLのみ
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && python3 tools/test_sfx.py"   # 焼き込んだ効果音表と旧合成の差（WSLのみ。gccはWindows側に無い）
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && python3 tools/make_font.py /tmp/cegen && gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I /tmp/cegen -I tools/hostshim -I main/hal -I main/ui -I main/text tools/test_codeedit.c tools/hostshim/hostshim.c main/ui/codeedit.c main/ui/paint.c main/ui/vimcmd.c main/text/jslex.c -o /tmp/t && /tmp/t"   # エディタの差分再描画と全面再描画が同じピクセルか（WSLのみ）
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && bash tools/build_pocket_text_test.sh && /tmp/test-pocket-text"   # pocket.input.text のセッション寿命を実物のQuickJSごとASanで（WSLのみ。番号を渡すと1件だけ）
python tools/pie/stalls.py              # PIEインラインasmの静的パイプライン解析
python tools/pie/test_kernels.py        # PIEカーネルを命令レベルで模擬実行しスカラーと全画素比較
python tools/pie/run_models.py          # カーネルが使う式の全域ビット一致証明
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && bash tools/build_lessons_test.sh && /tmp/test-lessons"   # TUTORIALの全章とPlaygroundの既定ソースを実物のQuickJSとpocket.kasaneで実行（WSLのみ）
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && bash tools/build_stress_app_test.sh && /tmp/test-stress-app"   # STRESS TESTを実物のQuickJSとpocket.kasaneで900フレーム（Kasaneが断るシーンを焼く前に、WSLのみ）
python tools/memlog.py --map build_api/cardputer_pocketjs.map            # DRAMの増減とファイル別内訳
python tools/memlog.py --map build_api/cardputer_pocketjs.map --port COM3 --check   # 実機の空きも記録し予算を検査
```
