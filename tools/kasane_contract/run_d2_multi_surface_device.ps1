param([string]$DiagBuild='build_d2_multi_surface',
      [string]$Out='.cache\d2-multi-surface-device-20260928')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal='build_megademo_install\cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$esp=@('-m','esptool','--chip','esp32s3','--port','COM3',
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $normal) -or
   !(Select-String -Path $cache -Pattern '^KASANE_D2_MULTI_SURFACE_PROBE:BOOL=ON$' -Quiet)) {
    throw 'D2 multi-surface diagnostic flag/image or normal image missing.'
}
if(Select-String -Path $cache -Pattern '^KASANE_D4_PIXEL_APP_PROBE:BOOL=ON$' -Quiet) {
    throw 'D2 and D4 VIDEO LAB probes must not share an image.'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    & $py tools\kasane_contract\run_d2_multi_surface_device.py --port COM3 --out (Join-Path $Out 'serial.log')
    if($LASTEXITCODE -ne 0){throw 'D2 multi-surface diagnostic failed.'}
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
