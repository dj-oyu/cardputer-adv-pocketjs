param([string]$DiagBuild='build_d6_sd_av_stream',
      [string]$Out='.cache\d6-sd-av-stream-device-20260928')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal='build_megademo_install\cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$homeCheck='tools\kasane_contract\check_home.py'
$esp=@('-m','esptool','--chip','esp32s3','--port','COM3',
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $normal) -or
   !(Select-String -Path $cache -Pattern '^KASANE_D6_SD_AV_STREAM_PROBE:BOOL=ON$' -Quiet)) {
    throw 'D6 SD AV diagnostic flag/image or normal image missing.'
}
if(!(Test-Path $homeCheck)){throw 'HOME_READY verification helper missing.'}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    & $py tools\kasane_contract\run_d6_sd_av_stream_device.py --port COM3 --out $Out
    if($LASTEXITCODE -ne 0){throw 'D6 SD AV diagnostic failed.'}
} finally {
    if($restore){
        & $py @esp write-flash 0x10000 $normal *> (Join-Path $Out 'restore-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal restore failed.'}
        & $py @esp verify-flash 0x10000 $normal *> (Join-Path $Out 'verify-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal flash verify failed.'}
        & $py $homeCheck
        if($LASTEXITCODE -ne 0){throw 'HOME_READY missing after restore.'}
    }
}
