param([string]$DiagBuild='build_flower_d1_device',
      [string]$NormalBuild='build_megademo_install',
      [string]$Out='.cache\flower-d1-device',
      [string]$Port='COM3')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal=Join-Path $NormalBuild 'cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
if(!(Test-Path $diag) -or !(Test-Path $normal) -or !(Test-Path $cache) -or
   !(Select-String -Path $cache -Pattern '^KASANE_FLOWER_D1_PROBE:BOOL=ON$' -Quiet)) {
    throw 'D1 diagnostic image/flag or normal image missing.'
}
$esp=@('-m','esptool','--chip','esp32s3','--port',$Port,
       '--before','usb-reset','--connect-attempts','3')
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-FileHash -Algorithm SHA256 $diag,$normal |
    Select-Object Path,Hash | Out-File -Encoding utf8 (Join-Path $Out 'image-hashes.txt')
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    & $py tools\flower_instrument_capture.py --port $Port --seconds 30 --backs 1 --out (Join-Path $Out 'flower.log') *> (Join-Path $Out 'capture.stdout.log')
    if($LASTEXITCODE -ne 0){throw 'FLOWER capture failed.'}
    $log=Get-Content (Join-Path $Out 'flower.log')
    $species=@($log | Where-Object { $_ -match 'FLOWER_D1: SPECIES id=' })
    if($species.Count -ne 14){throw "Expected 14 species reports, got $($species.Count)."}
    for($id=0;$id -lt 14;$id++){
        $matches=@($species | Where-Object { $_ -match "SPECIES id=$id name=" -and $_ -match 'lease=1 host_match=1' })
        if($matches.Count -ne 1){throw "Species $id hash/lease gate failed."}
    }
    if(!($log | Where-Object { $_ -match 'FLOWER_D1: SUMMARY passes=14 failures=0 count=14' })){
        throw 'FLOWER species summary failed.'
    }
    $pressure=@($log | Where-Object { $_ -match 'FLOWER_D1_HEAP: PRESSURE ready=1 ' })
    if($pressure.Count -ne 1){throw "Expected one real ready heap pressure, got $($pressure.Count)."}
    $fallback=@($log | Where-Object {
        $_ -match 'FLOWER_D1_ALLOC: PRESSURE ready=1 candidate=0 committed=1 fallback=1 before=([0-9a-fA-F]{8}) after=([0-9a-fA-F]{8})'
    })
    if($fallback.Count -ne 1){throw "Expected one committed fallback under pressure, got $($fallback.Count)."}
    if($fallback[0] -notmatch 'before=([0-9a-fA-F]{8}) after=([0-9a-fA-F]{8})' -or
       $Matches[1] -ne $Matches[2]){throw 'Committed FLOWER+rain repaint hash changed.'}
    $recovery=@($log | Where-Object {
        $_ -match 'FLOWER_D1_ALLOC: RECOVERY candidate=1 committed=1 hash=[0-9a-fA-F]{8}'
    })
    if($recovery.Count -ne 1){throw "Expected one recovered candidate ACK, got $($recovery.Count)."}
    if(!($log | Where-Object { $_ -match 'FLOWER_D1_HEAP: RELEASE ' })){
        throw 'Heap reservations were not released.'
    }
    $species | ForEach-Object { Write-Output $_ }
    $log | Where-Object { $_ -match 'FLOWER_D1_HEAP|FLOWER_D1_ALLOC|FLOWER_D1: SUMMARY' } |
        ForEach-Object { Write-Output $_ }
} finally {
    if($restore){
        & $py @esp write-flash 0x10000 $normal *> (Join-Path $Out 'restore-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal restore failed.'}
        & $py @esp verify-flash 0x10000 $normal *> (Join-Path $Out 'verify-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal flash verify failed.'}
        & $py tools\kasane_contract\check_home.py --port $Port
        if($LASTEXITCODE -ne 0){throw 'HOME_READY missing after restore.'}
    }
}
