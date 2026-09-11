# 05 メモリ確保台帳（allocator置き換え / L2aセグメント向け）

対象: `components/quickjs-ng/quickjs-ng/quickjs.c`（quickjs-ng 0.14.0 + immutable-buffer patch）と、ゲスト側の実装 `components/pocketjs_guest/src/guest.c`。すべて事実（コードが何をしているか）を記す。判断・推奨は末尾の「L2/L3/L5 に効く点」にのみ短くまとめる。

## 1. ゲスト側アロケータ（`JSMallocFunctions` の実装）

- `components/pocketjs_guest/src/guest.c:15-18` `allocation_header_t` は `union { size_t size; max_align_t alignment; }`。すべての確保の直前に1個置かれる自前ヘッダで、サイズを記録するだけ（ケーパビリティやマジックナンバーは持たない）。
- `guest.c:47-65` `guest_malloc`: `total = sizeof(allocation_header_t) + size` を `heap_caps_malloc()` で確保し、ヘッダに `size`（＝ユーザーが要求したバイト数そのもの、ヘッダやアロケータ内部の丸め込み分は含まない）を書き込み、`header + 1` を返す。`size == 0` または `size > SIZE_MAX - sizeof(header)` は `NULL`。
- `guest.c:54-59` PSRAM優先フラグ `guest->prefer_psram` が真なら `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` を先に試し、失敗（`NULL`）した場合のみ `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` にフォールバックする。`guest == NULL` のときもフォールバックのみ実行される（`JS_NewRuntime2` 内で `rt = mf->js_calloc(...)` を呼ぶ際、`opaque` はまだ有効な `guest` なので実際には `guest == NULL` 分岐は通常路には現れない）。
- 本ファームの実運用: `main/app_session.c:257` がメインアプリ用ゲストに `gc.prefer_psram=false` を明示しているため、メインアプリの全確保は常に `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 一本槍（PSRAMは搭載されていないのでこの機体ではどのみち内部DRAMのみ）。
- `guest.c:79-84` `guest_free`: ヘッダ位置に戻して `heap_caps_free()`。`pointer == NULL` は無視。
- `guest.c:86-89` `guest_usable_size`: ヘッダの `size` フィールド（＝**確保時に要求した値そのもの**）を返す。ESP-IDFのヒープ実装が内部でブロックサイズを切り上げていても、その余剰（スラック）は一切 quickjs 側に見せていない。
- `guest.c:91-107` `guest_realloc`: `pointer==NULL` なら `guest_malloc`、`size==0` なら `guest_free`+`NULL`。それ以外は **常に** 新規 `guest_malloc(size)` → `memcpy(min(old,new))` → `guest_free(old)` という「malloc+copy+free」で実装されている。`heap_caps_realloc()` は一切呼ばれておらず、その場拡張（in-place grow）は構造的に発生しない。
- `guest.c:109-115` `GUEST_ALLOCATOR` として `JSMallocFunctions` に登録: `js_calloc=guest_calloc`（`guest_malloc`+`memset`)、`js_malloc=guest_malloc`、`js_free=guest_free`、`js_realloc=guest_realloc`、`js_malloc_usable_size=guest_usable_size`。

### ヒープ上限（`MALLOC_CAP` フォールバックと結びつく強制値）

- `main/app_session.c:257` メインアプリ: `gc.heap_limit = 160*1024`（=163,840バイト）、`gc.stack_limit = 20*1024`、`gc.prefer_psram=false`。
- `main/app_session.h:49` `OVERLAY_GUEST_HEAP = (160*1024)`、`main/app_session.c:267` がオーバーレイセッションでも同じ値を上書き設定。
- **CLAUDE.mdの「JSゲストの上限は144KiB」という記述は、現在のソース値（160KiB=163,840B）と一致しない**（実測ではなくソース読み取りによる事実）。144KiBという数字自体がどこから来たか今回のスコープでは確認していない（未確認セクション参照）。
- `heap_limit` は `pocketjs_guest_create()`（`guest.c:230`）内で `JS_SetMemoryLimit(guest->runtime, config->heap_limit)` に渡り、`quickjs.c:2084-2087` の `rt->malloc_state.malloc_limit = limit;` にそのまま入る。`quickjs.c:1604,1630,1681` の上限チェックはすべて `malloc_size + 追加分 > malloc_limit - 1` で判定するため、実際に確保を拒否され始めるのは `malloc_size`（quickjs側のトラッキング値。後述）が `heap_limit - 1` を超えた瞬間であり、ESP-IDFヒープの実消費量そのものではない。

## 2. `js_malloc` / `js_realloc` / `js_free_rt` と `JSMallocState` の会計

- `quickjs.c:475-481`（`quickjs.h`）`JSMallocFunctions` は5関数ポインタ: `js_calloc(opaque,count,size)`, `js_malloc(opaque,size)`, `js_free(opaque,ptr)`, `js_realloc(opaque,ptr,size)`, `js_malloc_usable_size(ptr)`（`opaque` を取らない）。
- `quickjs.c:250-255` `JSMallocState { malloc_count, malloc_size, malloc_limit, opaque }`。`rt->malloc_state` として `JSRuntime` に1個だけ埋め込まれる（ランタイム全体でグローバルな単一カウンタ）。
- `quickjs.c:57-61` `MALLOC_OVERHEAD` は `__APPLE__` なら0、それ以外（ESP32含む）は **8**。この8は「実際のmallocヘッダのオーバーヘッド見積もり」として `malloc_size` の加減算に毎回加わる（実ヘッダのバイト数とは無関係に定数）。
- `quickjs.c:1589-1616` `js_calloc_rt`: `count*size` のオーバーフローチェック後、`malloc_size + count*size > malloc_limit - 1` なら `NULL`（`js_calloc` 自体は呼ばれない＝ここで拒否）。成功したら `malloc_count++`、`malloc_size += rt->mf.js_malloc_usable_size(ptr) + MALLOC_OVERHEAD`。**加算されるのはゲストの `usable_size()` の戻り値であり、要求サイズではない** — ただし本ファームの `guest_usable_size` は要求サイズをそのまま返すので、実質的には要求サイズ+8が積まれる。
- `quickjs.c:1618-1642` `js_malloc_rt`: `size==0` は `NULL`。同じ上限チェック、同じ会計。
- `quickjs.c:1644-1661` `js_free_rt`: `free_size = usable_size(ptr) + MALLOC_OVERHEAD` を `malloc_size` から引く。**`free_size > malloc_size` なら `printf` して `abort()`**（アンダーフロー検出、リカバリ不能な即死）。これはランタイム越しの外部からの `js_free_rt` 呼び出し順序ミスや、`usable_size` の値が確保時と解放時で食い違うケースを検出するガードであり、後者は今回のゲストallocatorでは発生しない（`guest_usable_size` はヘッダの固定値を読むだけ）。
- `quickjs.c:1663-1692` `js_realloc_rt`: `ptr==NULL` は `js_malloc_rt` へ、`size==0` は `js_free_rt` して `NULL`。`old_size = usable_size(ptr)` を確保前に読み、`malloc_size + size - old_size > malloc_limit - 1` で事前チェックしてから `rt->mf.js_realloc()` を呼ぶ。成功後は `malloc_size += usable_size(new_ptr) - old_size` で差分更新（`malloc_count` は変えない＝realloc は「個数」に対して中立という設計）。
- `quickjs.c:1953-1975` `JS_NewRuntime2`: `ms.malloc_limit=0`（無制限スタート）。`rt` 自体を `mf->js_calloc(opaque,1,sizeof(JSRuntime))` で確保し、`ms.malloc_count=1; ms.malloc_size = usable_size(rt)+MALLOC_OVERHEAD` を手動でインライン計上（`js_malloc_rt` を使わずに、というコメント通り）。**`rt->malloc_gc_threshold = 256*1024` を初期値として設定**（後述のGCトリガと直結、heap_limit=160KiBより大きい）。
- `quickjs.c:2084-2087` `JS_SetMemoryLimit` は `malloc_limit` を上書きするだけで、`malloc_gc_threshold` には触れない。

## 3. GCトリガの閾値（`malloc_gc_threshold`）

- `quickjs.c:1563-1582` `js_trigger_gc(rt, size)`: `force_gc = (malloc_size + size) > malloc_gc_threshold`。真なら `JS_RunGC(rt)` を呼び、その後 `malloc_gc_threshold = malloc_size + (malloc_size >> 1)`（GC後の生存量の1.5倍を次の閾値にする、典型的な世代非依存インクリメンタル閾値）。
- **`js_trigger_gc` の呼び出し箇所は `quickjs.c:5916` 一箇所のみ**（`JS_NewObjectFromShape` の先頭、`sizeof(JSObject)` を渡す）。つまり自動GCは「新規オブジェクト生成のたび」にしかチェックされない。文字列確保・配列拡張・シェイプ拡張・Promise生成など、オブジェクトを直接作らない確保経路は GC 発火のトリガにならない（それらの確保でヒープが逼迫しても、次にオブジェクトが1個作られるまで閾値チェックが走らない）。
- `main/app_session.c` と `components/pocketjs_guest/` を検索した限り、**`JS_SetGCThreshold()` も `JS_RunGC()` の手動呼び出しも本ファームのどこにも存在しない**（`grep -rn "SetGCThreshold\|GCThreshold\|JS_RunGC" main/ components/pocketjs_guest/` は0件、2026-09-12実施）。
- 初期閾値256KiBは、メインアプリの `heap_limit=160*1024`（160KiB）より大きい。`malloc_limit` による確保拒否は `malloc_size` が `heap_limit-1` を超えた時点で起きるため、**`malloc_size` が256KiBの閾値に到達する前に必ず160KiBの上限に先に当たる**。閾値を下げる呼び出しがどこにも無い以上、`js_trigger_gc` 内の `force_gc` 判定は「実行中ずっと false のまま」になる可能性が高い（＝閾値ベースの自動サイクルGCは通常運用では一度も発火しない）。この機体でオブジェクトの循環参照（弱参照で切れない循環）を解放できるのは、明示的な `JS_RunGC` 呼び出しが無い限り、参照カウントが0になる即時解放だけ、ということになる。**この結論は「呼び出し箇所を全部grepした」という事実からの直接の帰結であり、実測ではなくコード読解によるもの** — 動作中に本当に一度もGCが走らないかどうかは実機ログで確認していない。
- `quickjs.c:7283-7294` `JS_RunGC` 自体は `gc_decref` → `gc_scan` → `gc_free_cycles` の3パスで、いずれも既存の `rt->gc_obj_list` を走査するだけで新規確保を行わない（マーク&スイープが自分自身のために追加メモリを要求することはない、という設計）。ヒープが逼迫した状態でも安全に呼べる操作という前提が成り立っている。

## 4. オブジェクト種別ごとの確保

### JSObject 本体とシェイプ（`JSShape`）

- `quickjs.c:5910-5917` `JS_NewObjectFromShape`: `js_trigger_gc()` の直後に `p = js_malloc(ctx, sizeof(JSObject))`。オブジェクト本体は常に単独の固定長確保（プロパティ配列は別）。
- `quickjs.c:1014` 以降 `struct JSShape` はハッシュテーブル領域とプロパティ配列領域を「後ろに連続して」持つ可変長構造体（`get_shape_size(hash_size, prop_size)` で必要バイト数を計算し、`js_malloc` で1回に確保）。`sh->prop` 等は同じブロック内へのオフセットアクセスであり、外部への自己ポインタは持たない。
- `quickjs.c:5404-5429` `resize_shape_hash`: `rt->shape_hash`（ランタイム全体で1個、全シェイプが登録されるグローバルハッシュ表）を2倍成長で `js_mallocz_rt` → 全エントリ再リンク → 旧テーブル `js_free_rt`。**新しいブロックへの完全な差し替え**（realloc ではなく mallocz+free）。`js_new_shape2`（`quickjs.c:5484-5505`）が `2*(shape_hash_count+1) > shape_hash_size` で呼ぶ。
- `quickjs.c:5625-5691` `resize_properties`: プロパティ数を1.5倍成長（`new_size = max_int(count, sh->prop_size * 3/2)`）。
  - `p->prop`（`JSProperty` 配列、値そのものを保持）は `js_realloc` でその場拡張を試みる（`quickjs.c:5640`）。
  - シェイプのハッシュサイズも変える必要がある場合（`quickjs.c:5650-5675`）は **新しい `JSShape` ブロックを `js_malloc` で確保し `memcpy` で移し、古いブロックを `js_free`** する（実質的な「手動realloc」で、ポインタが必ず変わる）。呼び出し元は `*psh = sh` で新ポインタを受け取る契約になっている。
  - ハッシュサイズが変わらない場合（`quickjs.c:5676-5688`）は `js_realloc` でシェイプブロック自体をその場拡張しようとする（`sh_alloc = js_realloc(ctx, get_alloc_from_shape(sh), ...)`）——**この経路は quickjs 側の実装として `JSShape` ブロックの移動を realloc の返り値経由で許容している**。呼び出し元はすべて `psh`（ポインタへのポインタ）経由でアクセスするため、移動そのものは安全に扱われている。
  - `add_shape_property`（`quickjs.c:5765-`）が `prop_count >= prop_size` のときにこれを呼ぶ。
- `quickjs.c:5695-5763` `compact_properties`（削除済みプロパティの詰め直し）も同様に **新規 `js_malloc` → `memcpy` → 旧ブロック `js_free`** パターンで `JSShape` を作り直す。`p->prop` 側も末尾で `js_realloc` により縮小。
- `quickjs.c:5554-5581` `js_clone_shape`（プロトタイプ変更などでシェイプを複製する経路）も新規 `js_malloc` + `memcpy`。

### アトムテーブル（`rt->atom_array` / `rt->atom_hash`）

- `quickjs.c:273-278` `JSRuntime` の直接フィールド: `atom_hash_size`（2のべき）、`atom_count`、`atom_size`、`atom_count_resize`（次にハッシュ拡張すべきカウント）、`atom_hash`（ハッシュバケット配列）、`atom_array`（`JSAtomStruct*` の配列、添字がアトムID）。**ランタイムにつき1組だけの、実行中ずっと存在し続けるテーブル**（ゲスト1個＝ランタイム1個なので、ゲストのライフタイム全体で保持）。
- `quickjs.c:3172-3219` `atom_array` の初回拡張は `new_size = max_int(711, atom_size*3/2)` で、**最初の拡張でいきなり711エントリ**（コメント `quickjs.c:3178` に成長系列 `4 6 9 13 19 28 ... 711 1066 ...` が明記されている。JS_ATOM_COUNT (定義済みアトム504個以上)を1回のバッチ確保で賄うため）。`js_realloc_rt` でその場拡張を試みる（`quickjs.c:3186`、XXXコメントに「realloc2でslackを使うべき」と書かれている＝現状はslack未活用）。ポインタ配列なので32bit環境では `711 * 4B ≈ 2.8KB` がこの1回の確保で動く。
- `quickjs.c:2988-3017` `JS_ResizeAtomHash`: 新しいハッシュ配列を `js_mallocz_rt` で確保し、全既存エントリを再ハッシュしてから **旧配列を`js_free_rt`** （atom_arrayとは異なりmallocz+freeパターン、realloc未使用）。`JS_ATOM_COUNT_RESIZE`（`quickjs.c:3014`）で次回拡張カウントを更新。`atom_count >= atom_count_resize` になった時点（`quickjs.c:3273`）で `JS_ResizeAtomHash(rt, atom_hash_size*2)` が呼ばれる（2倍成長、atom_arrayの1.5倍とは異なる係数）。

### JSString / StringBuffer / ロープ文字列

- `quickjs.c:614-629` `struct JSString`: `JSRefCountHeader` + 長さ/幅/ハッシュ等のビットフィールド + `kind`（`NORMAL`/`SLICE`/`INDIRECT`等、2bit）+ `first_weak_ref`。文字データ本体は構造体の**直後**に続く（`kind==NORMAL`の場合、`quickjs.c:645-652` `strv()` が `&p[1]` を返す）。
- `quickjs.c:2223-2241` `js_alloc_string_rt`: `js_malloc_rt(rt, sizeof(JSString) + (max_len<<is_wide_char) + 1 - is_wide_char)` の単独確保。`kind = JS_STRING_KIND_NORMAL` 固定でここでは作る（スライス/ロープは別経路）。
- `quickjs.c:631-634` `JSStringSlice { JSString *parent; uint32_t start; }`: 親文字列の一部を指す「スライス」種別の文字列。`kind==SLICE` の場合、自前のバッファを持たず、**親 `JSString` への参照（refcount）を保持して間接参照**する（`quickjs.c:2274-2278` `js_free_string0`: スライス解放時は `js_free_string(rt, slice->parent)` を再帰的に呼ぶ、「1階層だけ再帰」とコメントされている）。
- `quickjs.c:636-643` `struct JSStringRope { header; len; is_wide_char; depth; JSValue left; JSValue right; }`: 文字列連結（`+`）で使われる**ロープ木**。左右の子は別々の `JSValue`（文字列 or 別のロープ）への参照で、**連結時にバッファをコピーしない**設計（quickjs-ng固有の最適化）。ロープ自体の確保サイズは固定（子への参照2個ぶんのみ）。
- `quickjs.c:4063-4070` `StringBuffer { ctx, str(JSString*), len, size, is_wide_char, error_status }`: 文字列を段階的に構築するための一時バッファ。`string_buffer_init2`（`quickjs.c:4076-4094`）が最初に `js_alloc_string(ctx,size,is_wide)` で確保。
- `quickjs.c:4140-4162` `string_buffer_realloc`: **`new_size = min(max(new_len, size*3/2), JS_STRING_LEN_MAX)`**（1.5倍成長、文字列長上限でクランプ）。`js_realloc2(ctx, s->str, sizeof(JSString)+バイト数, &slack)` で `JSString` ブロックそのものをその場拡張しようとし、余ったslackぶんだけ `new_size` を上乗せする（`quickjs.c:4159-4163`、slackは実際に活用されている数少ない経路）。
- `quickjs.c:4116-4138` `string_buffer_widen`: 8bit文字列を16bit(wide)へ昇格するときも `js_realloc2` でブロックを再確保し、8bit→16bitへその場でデータを展開しなおす。
- **注意**: `string_buffer_realloc`/`widen` は `JSString*` を直接 `js_realloc2` に渡している。本ゲストの `guest_realloc` は常に新規malloc+memcpy+freeなので、この“その場拡張”はゲスト環境では実質的に**必ず新しいブロックへの完全コピーになる**（quickjs.c側の意図＝slackで無駄なコピーを減らす、という前提がguest.c側の実装で部分的に無効化されている。ただし`js_realloc2`が返す`slack`自体はゲストの`guest_usable_size`が要求サイズをそのまま返す実装のため常に0になる——後述4章末を参照）。

### fast array（配列の高速パス）

- `quickjs.c:10334-10355` `expand_fast_array`: `new_size = old_size + old_size/2`（1.5倍、整数オーバーフローチェック付き）、`new_size = max(new_len, new_size)`。`js_realloc2(ctx, p->u.array.u.values, sizeof(JSValue)*new_size, &slack)` でその場拡張、slack分を追加で `p->u.array.u1.size` に反映。
- `quickjs.c:10359-10387` `add_fast_array_element`: `count+1 > u1.size` のときのみ `expand_fast_array` を呼ぶ（毎要素pushでの再確保を避ける、標準的な償却O(1)設計）。

### バイトコード・クロージャ・var_ref

- `quickjs.c:767-803` `struct JSFunctionBytecode`: `byte_code_buf`・`vardefs`・`closure_var`・`cpool` は「自己ポインタ」（構造体自身の末尾に確保された領域を指す）とコメントされている。`pc2line_buf`・`source` は**別確保**。
- `quickjs.c:37135-37198` `js_create_function`（パス2完了後の本体生成）: `function_size = sizeof(*b) + cpool_count*sizeof(JSValue) + (arg_count+var_count)*sizeof(JSVarDef) + closure_var_count*sizeof(JSClosureVar) + byte_code.size` を計算し、**`b = js_mallocz(ctx, function_size)` の単一確保**の中に `cpool`・`vardefs`・`closure_var`・`byte_code_buf` をオフセット計算で詰め込む（`quickjs.c:37151,37159,37176`：`(void*)((uint8_t*)b + offset)`)。コンパイラ側の一時配列（`fd->byte_code.buf`, `fd->args`, `fd->vars`, `fd->cpool`）はコピー後に `js_free` される（一時領域、長寿命ではない）。
- **`b->pc2line_buf`（`quickjs.c:37191`）と `b->source`（`quickjs.c:37196`）はこの単一確保に含まれず、別ブロックのまま `JSFunctionBytecode` にぶら下がる**。`quickjs.c:37184-37186` のコメントが「本来は構造体末尾に詰めてアロケーションオーバーヘッドを避けるべき」と upstream 自身が認めている未解決の最適化ポイント。
- **`JSFunctionBytecode` ブロック自身がどこかで `js_realloc` される箇所は見当たらない**（今回のgrep範囲では未確認、下記「未確認」参照）。`b->byte_code_buf` 等の自己ポインタは確保時に一度だけ計算されるため、**もし `b` を指す外側のブロックを移動的に再確保する設計にするなら、これらの内部自己ポインタを個別に再計算しない限り不整合になる**（事実として、生成時に絶対アドレスとして書き込まれ、以降どこにも再計算コードがない）。
- `quickjs.c:405-425` `struct JSVarRef`: `pvalue` は「未クローズ時」は `JSStackFrame` の `arg_buf`/`var_buf`（＝関数呼び出しの値スタック領域）を指し、「クローズ後」（`is_detached=true`）は自分自身の `value` フィールドを指す（`close_var_ref`、`quickjs.c:17883-17890`：`var_ref->value = js_dup(*var_ref->pvalue); var_ref->pvalue = &var_ref->value;`）。
- `quickjs.c:17582-17626` `get_var_ref`: 捕捉された変数のvar_refは `pvalue = &sf->arg_buf[var_idx]` または `&sf->var_buf[var_idx]` を直接指す（**ヒープではなく、関数呼び出しの値スタック領域そのもの**）。
- `quickjs.c:18221-18246`（`JS_CallInternal` 通常呼び出し経路）: `local_buf = alloca(alloca_size)` で **arg_buf/var_buf/stack_buf はホストのCスタック（＝FreeRTOSタスクスタック）上に確保される**。つまり通常の同期関数呼び出しでは、ローカル変数の実体はヒープではなくタスクスタック上にあり、`JSVarRef.pvalue` は一時的にそこを指す。
- `quickjs.c:21051-21093` `async_func_init`（ジェネレータ・async関数用）: `alloc_size = sizeof(JSValue)*max(local_count,1) + sizeof(JSVarRef*)*var_ref_count` を **`js_malloc(ctx, alloc_size)` でヒープに一括確保**し、`sf->arg_buf`・`sf->var_buf`・`sf->cur_sp`・`sf->var_refs` をすべてこの1ブロック内のオフセットとして設定する。これは「Cスタックが巻き戻ってもフレームを保持する」ために、async/generatorのフレームだけは最初からヒープ常駐にしてある、という設計（コメント `quickjs.c:21050`: "used by generator and async functions")。サスペンド中（`await`/`yield` で中断している間）ずっと生存する**長寿命確保**。
- `quickjs.c:21116-21138` `async_func_free`: 生存中の値を全部 `JS_FreeValueRT` してから `js_free_rt(rt, sf->arg_buf)` で単一ブロックをまとめて解放。

### legacy `arguments` オブジェクト（mapped arguments）のvar_refs表——レビュー指摘により追記

- `js_build_mapped_arguments`（`quickjs.c:16858-`）: legacy（非strict）関数の `arguments` オブジェクトを作る際、`argc > 0` なら `tab = js_malloc(ctx, sizeof(tab[0]) * argc)`（`quickjs.c:16882`）で**呼び出しのたびに** `JSVarRef*` の配列を1個確保する。このブロックは生成した `JS_CLASS_MAPPED_ARGUMENTS` オブジェクトの fast array 部分（`p->u.array.u.var_refs`）として使われ、寿命はそのオブジェクト自身に従う（解放は `js_mapped_arguments_finalizer`、02-frame-pointers.md 4節末尾参照）。
- 配列の各要素は個別の `JSVarRef` 確保を伴う: `arg_count` 個までは `get_var_ref(ctx, sf, i, true)`（`quickjs.c:16887`、フレームの `arg_buf[i]` を指す非detached版、02-frame-pointers.md 4節の`get_var_ref`と同じ実装——新規なら`js_malloc`、既存キャプチャがあれば再利用）、それを超える分（宣言仮引数より多い実引数）は `js_create_var_ref(ctx, true)`（`quickjs.c:16894`、最初から自立したdetached版）。
- **これらは「フレーム」自体の確保（2b節の`async_func_init`が確保する`sf->arg_buf`ブロック等）とは別枠の、フレームに**関連付いて**いるが独立したヒープ確保**——**フレームの生存期間には従わない**（`arguments` オブジェクト自身が生き残ればフレーム終了後もこの`tab`とその中の`JSVarRef`群は生存し続ける。非detached分は`close_var_refs`でdetach化されて自立する、02-frame-pointers.md 4節）。**L2aがセグメントを「フレーム1個ぶん」として設計する場合、この`tab`をフレームのセグメントに含めるかどうかは別途決める必要がある——含めれば`arguments`オブジェクトが生きている間セグメントを解放できなくなり、含めなければフレーム外の通常ヒープ確保として扱うことになる。**

### Promise / ジョブキュー

- `quickjs.c:55905-55927` `JSPromiseData`（本体、固定サイズ）・`JSPromiseFunctionDataResolved`（resolve/reject関数ペアで共有される小さな参照カウント構造体）・`JSPromiseReactionData`（`.then()`のハンドラ1個ぶん、リンクリストのノード）。いずれも `js_mallocz(ctx, sizeof(*s))` パターンの固定長確保（呼び出し例: `quickjs.c:56313`）。
- `quickjs.c:979-985` `JSJobEntry { link; ctx; job_func; argc; JSValue argv[]; }`: フレキシブル配列メンバ。`quickjs.c:2138-2148` `JS_EnqueueJob`: `js_malloc(ctx, sizeof(*e) + argc*sizeof(JSValue))` の単一確保、`argc` は呼び出し箇所ごとに2〜5（`quickjs.c:31846`が5、`41299`が1、`56079`/`56939`が5、`56252`が3、`64222`が2）。**マイクロタスク1個ぶんの短命確保**（実行後に解放される想定、`JS_ExecutePendingJob` 側の解放は今回未追跡＝未確認）。

## 5. realloc呼び出し箇所の全体像（非移動ポリシーとの関係）

`grep -n "js_realloc(\|js_realloc2(\|js_realloc_rt(\|js_realloc_array(\|js_resize_array("` は定義部を除いて61箇所ヒット（2026-09-12実施、行番号は上記grep結果のとおり）。性質で分類すると:

- **ランタイム生存期間・共有状態**（GC後も生き残り、ゲストが動いている間ずっと存在しうる）: `rt->atom_array`（`quickjs.c:3186`）, `ctx->class_proto`/`rt->class_array`（`quickjs.c:3905,3916`、`JS_NewClassID`系のケーパビリティ登録で伸びる）, `JSShape`（`quickjs.c:5640,5679,5758,10047`）, fast array の `u.array.u.values`（`quickjs.c:10347`）, `JSString`/`StringBuffer`（`quickjs.c:4126,4159,4439`）, `JSBigInt`（`quickjs.c:12763,12803`）, resizable `ArrayBuffer` の `abuf->data`（`quickjs.c:60151,60230`）, Map/Setの `s->hash_table`（`quickjs.c:54389`、失敗時は**エラー報告なしで黙って諦める**とコメントあり `quickjs.c:54383`）。
- **パーサ・コンパイラの一時領域**（`JS_Eval` 実行中だけ存在し、`js_create_function` で最終形にコピーされた後 `js_free` される、または例外時に破棄される）: `fd->label_slots`（`24034`）, `fd->cpool`（`24103`）, `fd->vars_htab`（`24157`）, `fd->scopes`（`24351,24357`）, `fd->vars`（`24422`）, `fd->args`（`24510`）, `s->global_vars`（`24527`）, モジュール関連の `m->req_module_entries`/`export_entries`/`star_export_entries`/`import_entries`（`30336,30383,30412,32662`）, `s->array`（`30724`）, `s->modules`/`s->exported_names`（`30925,30938`）, `exec_list->tab`（`31912`）, `m1->async_parent_modules`（`32164`）, `s->closure_var`（`33559`、コンパイラ側のクロージャ変数配列、実行時の`JSClosureVar`とは別物）, `s->pc_stack`（`36772`）, `b->pc2line_buf`（`37191`、最終成果物だが別ブロックのまま）, `s->object_tab`/`atom_to_idx`/`idx_to_atom`（バイトコードシリアライザ関連、`38508,38699,38709`）, `s->sab_tab`（`39152,40479`）, `s->objects`（`39824`）。
- **式評価中の一時バッファ**（関数1回の呼び出し内で完結）: `Array.prototype.sort` の `ValueSlot` 配列（`45345`）, `ValueBuffer`（正規表現マッチ結果などに使う4要素インライン→ヒープ切替バッファ、`51082,51087`）, JSONやObject列挙の `po->entries`/`pr->u.array.elements`（`51746,51933`）, Base64/Hex関連の `bs`（`60151`、ArrayBuffer.transferとも重複）。

**非移動（non-moving）ポリシーへの実務的な含意（事実の列挙のみ）**:
- `JSShape` は「その場realloc」（`quickjs.c:5679`）と「新規malloc+memcpy+free」（`quickjs.c:5654,5719`）の両方の経路を持ち、**どちらの場合もポインタが変わりうる**。呼び出し元は必ず `JSShape**`（`psh`）経由で受け取り直す契約になっている。
- `JSString`（`string_buffer_realloc`/`widen`）と `JSObject.u.array.u.values`（`expand_fast_array`）も同様に `js_realloc2` を呼んでおり、**quickjs.c自身の設計は「ブロックは移動しうる」前提で書かれている**（呼び出し元は常に戻り値を新しい所有ポインタとして扱っている）。
- 一方で `JSFunctionBytecode` の内部自己ポインタ（`byte_code_buf`/`vardefs`/`closure_var`/`cpool`）は生成時に一度だけ絶対アドレスとして書き込まれ、その後これらを再計算するコードは見当たらない（4章参照）。**`JSFunctionBytecode` ブロック自体を再配置する設計にする場合は、quickjs.c側の変更なしには対応できない**（今回のgrep/read範囲内での事実）。
- `JSVarRef.pvalue` は未クローズ時にスタック領域（Cスタック上の `alloca` 領域、または async/generatorフレームのヒープ領域）を直接指す生ポインタであり、**GCヒープの再配置とは別の「スタック移動」問題**を持つ（`docs/quickjs-freertos-vm-spec.md` の "movable stack" 論点と直接関係する事実として記録するが、判断はしない）。
- 唯一「移動時のポインタ修復（fixup）」が実装済みの例は `js_array_buffer_update_typed_arrays`（`quickjs.c:60053-60089`）: resizable ArrayBuffer の `abuf->data` を `js_realloc` した後、`abuf->array_list` にぶら下がる全 `JSTypedArray`/`DataView` の `p->u.array.u.ptr` を新しいアドレスで書き直す、という「参照側リストを歩いて生ポインタを貼り替える」パターンがすでに存在する。

## 6. slack活用とゲストアロケータの相互作用（事実）

- quickjs.c側は `js_realloc2` を通じて「実際に確保できたサイズ（`usable_size`）が要求サイズより大きければ、その差分（slack）を追加容量として使う」という最適化を複数箇所（StringBuffer, fast array, atom_array=未使用コメントあり）に実装している。
- 本ゲストの `guest_usable_size`（`guest.c:86-89`）はヘッダに書いた**要求サイズそのもの**を返すため、実際にESP-IDFのヒープアロケータが内部で切り上げた余白があっても、**quickjs側からは常にslack=0として見える**。つまり `js_realloc2` のslack活用コード自体は生きているが、このゲスト実装のもとでは常に無効化されている（＝1.5倍成長アルゴリズムの「たまたま多く取れた分を使い切る」効果が出ない）。
- 加えて `guest_realloc` が常に新規malloc+copy+freeである（1章参照）ため、上記の「その場拡張」を意図したquickjs側のコード（`JSShape`のin-place realloc分岐、fast arrayのexpand等）は、このゲストの上では**意味的には動くが、実装としては常にフルコピーを伴う**。「1.5倍ずつ増やして再確保回数を減らす」という償却コストの意図そのものは保たれるが、「その場で伸ばせるときは伸ばす」という最適化は失われている。

## 未確認

- `JSFunctionBytecode`（`b`）自体を対象にした `js_realloc` 呼び出しが本当に一つも無いか、61件のrealloc箇所の分類以外の経路（マクロや別名関数経由）まで含めて確認していない。
- `JS_ExecutePendingJob`（`JSJobEntry` の実行と解放）を実装コードとして読んでいない。`argv[]` に確保されたJSValueの解放漏れ・二重解放の有無は未確認。
- `js_realloc_rt` の61件のうち、パーサ/コンパイラ由来に分類したものが本当にすべて「`JS_Eval` 完了までに解放される一時領域」であるか、個別に最後の解放コードまで追ってはいない（`js_free_function_def` 相当の後始末関数は名前ベースで存在を確認していない）。
- `sizeof(max_align_t)`（`allocation_header_t` のサイズを決める）をESP32-S3のツールチェーンヘッダで実際に確認していない。8バイトだろうという推定（Xtensa/RISC-V32のABIで一般的な値）であり、実測ではない。
- CLAUDE.mdに書かれている「JSゲストの上限は144KiB」という数値の出どころ（過去のコミット、別のビルド構成、単なる書き間違い）は未調査。現在の `main/app_session.c` / `app_session.h` の値（160KiB）とだけ突き合わせた。
- `rt->class_array`/`ctx->class_proto` の拡張（`quickjs.c:3897-3924`）が、`pocket_api_register()` などケーパビリティ登録のたびに実際何回発火するか（＝ `JS_CLASS_INIT_COUNT` の初期値と実際登録されるクラス数の関係)は未確認。
- Map/Set の `map_hash_resize`（`quickjs.c:54377-54393`）が失敗時に「エラー報告なしで諦める」とコメントされている動作が、`malloc_limit` 到達時に実際どう見えるか（例外にならず単に古いハッシュテーブルのまま動き続けるのか）を実機・ホストいずれでも検証していない。
- GCが実行中一度も発火しない、という3章の結論は静的なgrep・コードパスの読解のみに基づく（推定ではなく事実の連鎖からの論理的帰結だが、実機ログでの確認はしていない）。
- **本台帳群の行番号の基準:** 本ファイルの`quickjs.c`引用は `git rev-parse HEAD`（本ワークツリーでは `d9ef1f9`）に、`apps/vmprobe/` 計測フック用の未コミット差分（`quickjs.c`に+59行）が当たった状態を読んで書かれている（追記分は本追記時点のセッションで直接確認、既存部分がどの版を見て書かれたかは記録が無い）。この差分は今後コミットされる、あるいは別セッションが `quickjs.c` を編集すれば再びずれるので、file:line は不変の参照先として扱わないこと。

## L2/L3/L5 に効く点（短い列挙、判断は書かない）

- `guest_realloc` が常にfull copyである点は、L2で独自アロケータに差し替える際の比較基準になる（現状は「realloc最適化ゼロ」が既定値）。
- `guest_usable_size` がslackを返さない実装なので、L2側で本当にslackを提供したいなら `guest_usable_size` も合わせて書き換える必要がある。
- `JSFunctionBytecode` の内部自己ポインタと `JSVarRef.pvalue` のスタック直接参照は、L2a（セグメント方式）やmovable stackの設計時に「動かせない/動かす場合は修復が要る」対象として扱う必要がある。
- `js_trigger_gc` の閾値256KiB初期値とheap_limit(160KiB)の大小関係、および `JS_SetGCThreshold`/`JS_RunGC` が呼ばれていない事実は、L2/L3でメモリ逼迫時の挙動を設計する際の前提条件になる。
- `js_array_buffer_update_typed_arrays` は「移動後にリストを歩いて参照を貼り替える」既存パターンとして、L2の移動的な確保方式を検討する際の参考実装になりうる。
