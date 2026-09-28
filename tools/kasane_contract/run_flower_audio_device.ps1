param([string]$DiagBuild='build_proc_grid_scan_device',
      [string]$Out='.cache\flower-audio-20260928')
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal='build_megademo_install\cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$esp=@('-m','esptool','--chip','esp32s3','--port','COM3',
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $normal) -or
   !(Select-String -Path $cache -Pattern '^KASANE_FLOWER_AUDIO_PROBE:BOOL=ON$' -Quiet)) {
    throw 'Diagnostic FLOWER audio flag/image or normal image missing.'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    & $py tools\flower_instrument_capture.py --port COM3 --seconds 19 --backs 1 --out (Join-Path $Out 'flower.log') *> (Join-Path $Out 'capture.stdout.log')
    if($LASTEXITCODE -ne 0){throw 'FLOWER capture failed.'}
    $lines=@(Select-String -Path (Join-Path $Out 'flower.log') -Pattern 'FLOWER_AUDIO_PROBE: tone=[01] available=[01]')
    $on=@($lines | Where-Object { $_.Line -match 'tone=1' })
    $off=@($lines | Where-Object { $_.Line -match 'tone=0' })
    if($on.Count -lt 2 -or $off.Count -lt 2){throw "Too few audio A/B windows: on=$($on.Count) off=$($off.Count)"}
    if(@($lines | Where-Object { $_.Line -notmatch 'available=1' }).Count){
        throw 'Audio codec was unavailable during the A/B observation.'
    }
    $requests=@(Select-String -Path (Join-Path $Out 'flower.log') -Pattern 'FLOWER_AUDIO_PROBE: tone_request=(-?\d+)')
    $accepted=@($requests | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 0 })
    if($accepted.Count -lt $on.Count -or $accepted.Count -ne $requests.Count){
        throw "Tone requests were not accepted for every on-window: accepted=$($accepted.Count) on=$($on.Count)."
    }
    $lines | ForEach-Object { $_.Line }
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
