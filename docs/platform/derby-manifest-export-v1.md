# DERBY manifest export v1 の FW 受け口

PR37 `a89f891b8f05d7e161007f91beaf7d0fe20c484a` を基点にした別候補。
通常 main、測定済み J1 b561、PR37 の head は変更しない。FW/runtime、source 登録、配置、実機操作も対象外。

## 受理する束縛

`E = manifest_export` は次の4 fieldだけを持つ。schema は bool/float ではなく厳密な整数1、
contract は `broker-build-manifest-export-v1`、digest は小文字64桁、manifest は JSON object とする。
未知 schema/contract、追加 field、null、重複 JSON key、不正な commitment から legacy へ fallback しない。

```json
{
  "schema_version": 1,
  "contract": "broker-build-manifest-export-v1",
  "original_manifest_sha256": "<original canonical manifest digest>",
  "manifest": {"...": "the existing redacted public manifest"}
}
```

`H` は UTF-8 の canonical JSON（sort_keys=true、separators=(',', ':')、ensure_ascii=true、
NaN/Infinity 不許可）の SHA-256。通常の `digest()` は変更せず、v1 元 digest を使う比較箇所だけを分離する。

```text
H(E) = response.manifest_export_sha256 = flash_plan.manifest_export_sha256
E.original_manifest_sha256 = flash_plan.manifest_sha256
                          = planned/captured collector_binding.manifest_sha256
H(flash_plan) = succeeded flash_job.plan_hash
             = planned/captured collector_binding.flash_plan_hash
```

成功 job の plan hash は、owner 認可済み durable 記録から別途保持し、
`--expect-flash-plan-sha256` へ明示する。export や変更後 plan から期待値を生成し直さない。
JSON archive 全体と期待 hash を一緒に偽造すれば、hash だけでは認証できない。
受理の主張は owner-exported 記録の offline 整合性に限定する。

既存の job/owner/device/source、詳細/DMA 両軸、entry、外側 proof、BIN/ELF、raw capture、
END→EXIT→STOP→HOME の全検査を維持する。collector binding に新 field を加えず、既存の
`flash_plan_hash` を介して新 commitment に束縛する。

DMA public proof は raw `cmake_cache` を受け付けず、`cache_evidence` の3 fieldだけを検査する。
`retained_private` は厳密な true、bytes は厳密な整数1〜131072、sha256 は小文字64桁。
hash は actual local proof の DMA/build_mode/build_measure cache hash と一致し、CLI では元 cache の bytes とも一致する。
内部 manifest 本文を再構成したとは主張せず、結果は `original_manifest_recomputed=false` とする。

## CLI

legacy の `--manifest` は元 manifest を hash 検証できる J1 専用として維持し、結果の field も変更しない。
commitment のない秘匿済み DMA 記録は拒否する。旧 plan/receipt に新 field を補完しない。

v1 では `--manifest` の代わりに `--manifest-export <owner-read-build-logs.json>` を指定する。
入力は `manifest_export` と sibling `manifest_export_sha256` を含む読み取り応答。
従来の top-level `manifest` も存在する場合は wrapper 内の manifest と一致を要求する。
ログ cursor 等の metadata は検証権限・成功の根拠に使わない。

従来の build/raw/transport/flash/collection 引数に次を指定する。値は対象記録のものを使う。

```text
--manifest-export <owner-read-build-logs.json>
--expect-flash-plan-sha256 <separately-retained-successful-job-plan-hash>
--expect-head <actual-firmware-full-head>
--expect-detail OFF|ON
--expect-dma OFF|ON
--expect-entry-sha256 <expected-policy-entry-digest>
```

最後の DMA/entry 引数は対象 lane に応じる。host code の head を既存記録の FW head と取り違えない。
private record/cache を公開せず、原本を変更しない。

## 検証と未完の合流 gate

- legacy r2 全件と、六つの J1/DMA 選択・schema/型/null/追加 field・cache evidence・固定 plan hash の負例を実行する。
- export 改変＋再 hash、owner/job/device/source 差替え、plan 再 hash、commitment 削除は、元の期待 plan hash を固定して拒否を確認する。
- 四構成 target は actual BIN/ELF/cache に対する本物 CLI と完全合成の記録を使う。各構成で成功1件と
  anchor/null/export 再 hash/plan-owner 再 hash の負例4件、原本 hash 不変を検査する。実機記録とは呼ばない。
- MCP59 `c85412b2…` を使う合成テストの export projection は明示的な test fixture。
  新 MCP producer の実装・durable export・plan validator を通した合流検証とは区別する。
- MCP 担当の producer head 到着後、同じ canonical vector と実 export producer を使った合流を別途確認する。
  ログ失効/合成再起動後の同 digest、durable 破損、読取中の認可変更による返却拒否は MCP 側の gate。

source 実受理・登録・配置、実機性能/LCD/heap/stack、通常 main の受入は引き続き別段階とする。
