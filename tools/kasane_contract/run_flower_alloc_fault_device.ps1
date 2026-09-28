param([string]$DiagBuild='build_proc_grid_scan_device',
      [string]$Out='.cache\flower-alloc-fault-20260928')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal='build_megademo_install\cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$esp=@('-m','esptool','--chip','esp32s3','--port','COM3',
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $normal) -or
   !(Select-String -Path $cache -Pattern '^KASANE_FLOWER_ALLOC_FAULT_PROBE:BOOL=ON$' -Quiet) -or
   !(Select-String -Path $cache -Pattern '^KASANE_FLOWER_FRAME_PROBE:BOOL=OFF$' -Quiet)) {
    throw 'Diagnostic FLOWER fault flags/image or normal image missing.'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    & $py tools\flower_instrument_capture.py --port COM3 --seconds 12 --backs 1 --out (Join-Path $Out 'flower.log') *> (Join-Path $Out 'capture.stdout.log')
    if($LASTEXITCODE -ne 0){throw 'FLOWER capture failed.'}
    $faults=@(Select-String -Path (Join-Path $Out 'flower.log') -Pattern 'FLOWER_ALLOC_FAULT: injected=1 candidate=0 committed=1 fallback=1')
    if($faults.Count -lt 2){throw "Expected repeated committed-frame fallback, got $($faults.Count)."}
    $faults | ForEach-Object { $_.Line }
} finally {
    if($restore){
        & $py @esp write-flash 0x10000 $normal *> (Join-Path $Out 'restore-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal restore failed.'}
        & $py @esp verify-flash 0x10000 $normal *> (Join-Path $Out 'verify-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal flash verify failed.'}
        & $py tools\kasane_contract\check_home.py
        if($LASTEXITCODE -ne 0){throw 'HOME_READY missing after restore.'}
    }
}
