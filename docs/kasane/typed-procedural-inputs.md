# Typed procedural input candidate

基点: vm/main 91801cf。既存アプリの提出経路は変更しない。

`draw(handle, inputs)` は従来の Array と Float64Array（0..8要素）を受ける。短い入力は0埋めし、doubleからfloatへの丸めは従来と同じ。Float32Arrayは受けない。

`drawBulk(handles, inputs)` は Int32Array の live handle 1..8個と、その順に各8個の値を並べた Float64Array を受ける。小chunkを再利用し、全frameぶんの入力を保持しない。通常Arrayの逐次drawと同じplan/segment順を使う。

両bufferは通常ArrayBufferのみ。SharedArrayBuffer、detach済みview、NaN/Inf、float overflow、過大入力を拒否する。typed viewの取得からcopy完了まではuser callbackがない。bulkはhandle/全入力を検証し終えてからnative実行する。事前拒否はcandidateを変えない。途中実行/LIMIT失敗はbuildingを解除してcandidateをcommit不可にする。committed/pending、ACK/repairの所有権は変更しない。復旧にはbeginFrameが必要。

追加の永続native確保はない。bulk入力scratchは最大256 B、handle/slot配列は別に必要。JS側の8行bufferは512 B＋handles32 Bとobject headerを保持する。単体typed入力なら64 B＋header。RAM利益・FPS利益は未測定であり、全frameの入力保持へ置き換えることは推奨しない。

検証: real QuickJS adapterの既存48 frame、複数surface、IO repairに加え、Array/typed単体/bulkのRGB565一致、16回反復、NaN/Inf/overflow、stale handle、行長、chunk上限を確認する。hostと実機はJSValue/allocatorが異なる。実機FPS、断片化、最悪turn時間は未検証。COM3を使わない。

追加検証: 空Float64Arrayの0埋め、非zero offset view、±0/subnormal/float max丸め近傍、8行異色重複planの順序、後半handle/NaN拒否時のcandidate無変更。最大256 Bは入力scratchのみで、handle配列32 Bとslot pointer配列（実機32 B、64bit host64 B）、局所変数が別途stackを使う。
