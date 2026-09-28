param([string]$DiagBuild='build_proc_grid_scan_device',
      [string]$Out='.cache\d6-overlay-video-20260928',
      [switch]$CombinedFaults,[switch]$AudioTone,[switch]$SdMp3)
$ErrorActionPreference='Stop'
$py='C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag=Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal='build_megademo_install\cardputer_pocketjs.bin'
$cache=Join-Path $DiagBuild 'CMakeCache.txt'
$homeCheck='tools\kasane_contract\check_home.py'
$esp=@('-m','esptool','--chip','esp32s3','--port','COM3',
       '--before','usb-reset','--connect-attempts','3')
if(!(Test-Path $diag) -or !(Test-Path $normal) -or
   !(Select-String -Path $cache -Pattern '^KASANE_D6_VIDEO_OVERLAY_PROBE:BOOL=ON$' -Quiet)) {
    throw 'D6 video overlay diagnostic flag/image or normal image missing.'
}
if($CombinedFaults){
    foreach($flag in @('KASANE_P0_PROBE','KASANE_P5_NOTICE_PROBE',
                      'KASANE_P5_LOWHEAP_PROBE','KASANE_P5_OVERLAY_REPAIR_PROBE')){
        if(!(Select-String -Path $cache -Pattern "^${flag}:BOOL=ON$" -Quiet)){
            throw "D6 combined diagnostic requires $flag."
        }
    }
}
if($AudioTone -and !(Select-String -Path $cache -Pattern '^KASANE_FLOWER_AUDIO_PROBE:BOOL=ON$' -Quiet)){
    throw 'D6 tone diagnostic requires KASANE_FLOWER_AUDIO_PROBE.'
}
if(!(Test-Path $homeCheck)){throw 'HOME_READY verification helper missing.'}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$restore=$false
try {
    $restore=$true
    & $py @esp write-flash 0x10000 $diag *> (Join-Path $Out 'flash-diagnostic.log')
    if($LASTEXITCODE -ne 0){throw 'Diagnostic flash failed.'}
    $measure=@('tools\overlay_device_test.py','--port','COM3','--out',$Out,
               '--d6-video-seconds',$(if($SdMp3){'10'}else{'9'}))
    if($CombinedFaults){$measure+=@('--d6-combined-faults','--p1-nav-alias')}
    if($AudioTone){$measure+='--d6-audio-tone'}
    if($SdMp3){$measure+='--d6-sd-mp3'}
    & $py @measure
    if($LASTEXITCODE -ne 0){throw 'D6 overlay diagnostic failed.'}
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
