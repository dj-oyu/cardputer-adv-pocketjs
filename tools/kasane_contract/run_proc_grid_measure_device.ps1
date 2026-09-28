param([int]$Runs = 5, [string]$Out = '.cache\kasane-grid-profile-20260928-verified',
      [string]$DiagBuild = 'build_proc_grid_device',
      [switch]$MeasureMegaDemo, [switch]$MeasureFlower,
      [switch]$D4Scene,
      [int]$FlowerSeconds = 14)
$ErrorActionPreference = 'Stop'
$py = 'C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe'
$diag = Join-Path $DiagBuild 'cardputer_pocketjs.bin'
$normal = 'build_megademo_install\cardputer_pocketjs.bin'
if ($Runs -lt 1) { throw 'Runs must be positive.' }
if ($MeasureFlower -and $FlowerSeconds -lt 6) { throw 'FlowerSeconds must be at least 6.' }
$out = $Out
$esp = @('-m', 'esptool', '--chip', 'esp32s3', '--port', 'COM3',
         '--before', 'usb-reset', '--connect-attempts', '3')
if (!(Test-Path $diag) -or !(Test-Path $normal)) {
    throw 'Build both diagnostic and normal images before flashing.'
}
if (!(Select-String -Path (Join-Path $DiagBuild 'CMakeCache.txt') -Pattern '^KASANE_PROC_DEVICE_PROBE:BOOL=ON$' -Quiet) -or
    !(Select-String -Path build_megademo_install\CMakeCache.txt -Pattern '^KASANE_PROC_DEVICE_PROBE:BOOL=OFF$' -Quiet)) {
    throw 'Diagnostic/normal CMake options do not match.'
}
if ($MeasureFlower -and !(Select-String -Path (Join-Path $DiagBuild 'CMakeCache.txt') -Pattern '^KASANE_FLOWER_FRAME_PROBE:BOOL=ON$' -Quiet)) {
    throw 'FLOWER frame probe is not enabled in the diagnostic image.'
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$restore = $false
try {
    $restore = $true
    & $py @esp write-flash 0x10000 $diag *> "$out\flash-diagnostic.log"
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic flash failed.' }
    for ($run = 1; $run -le $Runs; $run++) {
        $log = "$out\grid-measure-$run.log"
        $probe = @('tools\kasane_contract\run_proc_compiler_device.py',
                   '--port','COM3','--out',$log,'--binary',$diag)
        if ($D4Scene) { $probe += '--d4-scene' }
        & $py @probe
        if ($LASTEXITCODE -ne 0) { throw "Grid diagnostic run $run failed." }
        Select-String -Path $log -Pattern 'GRID_TIME|GRID_DYNAMIC_TIME|GRID_BROADCAST_TIME|GRID_JS_TIME|GRID_JS PASS|GRID PASS|GRID_SCAN_TIME|GRID_SCAN PASS|KSN_COMPILER: PASS' |
            ForEach-Object { $_.Line }
    }
    if ($MeasureMegaDemo) {
        & $py tools\kasane_contract\measure_megademo_menu_device.py --port COM3 --out "$out\megademo-diagnostic.log" --runs 2
        if ($LASTEXITCODE -ne 0) { throw 'MEGADEMO diagnostic measurement failed.' }
    }
    if ($MeasureFlower) {
        $flowerLog = "$out\flower-frame.log"
        & $py tools\flower_instrument_capture.py --port COM3 --seconds $FlowerSeconds --out $flowerLog --backs 1
        if ($LASTEXITCODE -ne 0) { throw 'FLOWER frame capture failed.' }
        $probeLines = @(Select-String -Path $flowerLog -Pattern 'FLOWER_FRAME_PROBE: arm=')
        if ($probeLines.Count -lt 3) { throw 'FLOWER frame probe yielded too few windows.' }
        $probeLines | ForEach-Object { $_.Line }
    }
} finally {
    if ($restore) {
        & $py @esp write-flash 0x10000 $normal *> "$out\restore-normal.log"
        if ($LASTEXITCODE -ne 0) { throw 'Normal firmware restore failed.' }
        & $py @esp verify-flash 0x10000 $normal *> "$out\verify-normal.log"
        if ($LASTEXITCODE -ne 0) { throw 'Normal firmware verification failed.' }
        & $py tools\kasane_contract\check_home.py
        if ($LASTEXITCODE -ne 0) { throw 'HOME_READY missing after restore.' }
    }
}
