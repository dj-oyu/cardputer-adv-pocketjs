param([string]$DiagBuild='build_d5_file_stream',
      [string]$NormalBin='build_megademo_install\cardputer_pocketjs.bin',
      [string]$Out='.cache\d5-sd-stream-device',
      [string]$Port='COM3')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$runner='tools\kasane_contract\run_d5_sd_stream_device.py'
$homeCheck='tools\kasane_contract\check_home.py'
$esp=@('-m','esptool','--chip','esp32s3','--port',$Port,
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $NormalBin) -or
   !(Test-Path $runner) -or !(Test-Path $homeCheck) -or
   !(Select-String -Path $cache -Pattern '^KASANE_D5_SD_STREAM_PROBE:BOOL=ON$' -Quiet)) {
    throw 'D5 diagnostic flag/image, normal image, or runner missing.'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
"diagnostic_sha256=$((Get-FileHash $diag -Algorithm SHA256).Hash)" |
    Set-Content (Join-Path $Out 'images.txt')
"normal_sha256=$((Get-FileHash $NormalBin -Algorithm SHA256).Hash)" |
    Add-Content (Join-Path $Out 'images.txt')
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic app flash failed.'}
    & $py $runner --port $Port --out (Join-Path $Out 'serial.log') *> (Join-Path $Out 'runner.log')
    if($LASTEXITCODE -ne 0){throw 'D5 SD stream runner failed.'}
} finally {
    if($restore){
        & $py @esp write-flash 0x10000 $NormalBin *> (Join-Path $Out 'restore-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal app restore failed.'}
        & $py @esp verify-flash 0x10000 $NormalBin *> (Join-Path $Out 'verify-normal.log')
        if($LASTEXITCODE -ne 0){throw 'Normal app flash verify failed.'}
        & $py $homeCheck --port $Port *> (Join-Path $Out 'home-ready.log')
        if($LASTEXITCODE -ne 0){throw 'HOME_READY missing after normal restore.'}
    }
}
Get-Content (Join-Path $Out 'runner.log')
