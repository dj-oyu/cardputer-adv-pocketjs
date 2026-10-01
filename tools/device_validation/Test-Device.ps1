#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [Parameter(Mandatory=$true)][ValidateSet('baseline','candidate')][string]$Role,
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [ValidateRange(1,20)][int]$Cycles = 3,
    [switch]$Grid,
    [switch]$AllowDevice,
    [switch]$ConfirmRunningImage
)
. (Join-Path $PSScriptRoot 'Common.ps1')
if (!$AllowDevice -or !$ConfirmRunningImage) {
    throw 'Both -AllowDevice and -ConfirmRunningImage are required after manually rebooting the flashed image.'
}
$RunRoot = (Resolve-Path -LiteralPath $RunRoot).Path
$run = Read-Run $RunRoot
if ($Role -eq 'candidate' -and $run.candidate -eq 'F') { throw 'F has no runtime precompiled loader; device testing is excluded.' }
$entry = Get-Artifact $run $Role
$flash = Get-Content -Raw -LiteralPath (Join-Path $RunRoot 'last-flash.json') | ConvertFrom-Json
if ($flash.status -ne 'flash-verified-awaiting-manual-reboot' -or $flash.role -ne $Role -or $flash.port -ne $Port -or $flash.binarySha256 -ne $entry.binarySha256) {
    throw 'No matching successful flash verification. Use Flash-Validation.ps1 for this role and port first.'
}
$python = Get-IdfPython
$out = New-StageDirectory $RunRoot "device-$Role"
$result = [ordered]@{role=$Role; sourceSha=$entry.sha; binarySha256=$entry.binarySha256; port=$Port; status='preflight'; runningImage='user attested after verified flash'; physicalChecks='manual, not assessed'; startedUtc=[DateTime]::UtcNow.ToString('o'); failure=$null}
$lock = $null
try {
    Invoke-Logged $python @('-c','import serial; print(serial.__version__)') (Join-Path $out 'pyserial-version.log')
    $lock = Enter-DeviceLock $Port
    Invoke-Logged $python @((Join-Path $PSScriptRoot 'safe_smoke.py'),'--allow-device','--port',$Port,'--cycles',"$Cycles",'--out',(Join-Path $out 'hello')) (Join-Path $out 'hello-console.log')
    if ($Grid) {
        # Use each immutable candidate's app and matching measurement runner.
        $gridScript = Join-Path $entry.worktree 'tools/kasane_contract/measure_grid_pipeline_device.py'
        $gridLog = Join-Path $out 'grid-serial.log'
        Invoke-Logged $python @($gridScript,'--port',$Port,'--out',$gridLog) (Join-Path $out 'grid-summary.log')
        Invoke-Logged $python @((Join-Path $PSScriptRoot 'safe_smoke.py'),'--check-grid-log',$gridLog) (Join-Path $out 'grid-equality.log')
    }
    $result.status = if ($Grid) { 'hello-and-grid-markers-passed' } else { 'hello-markers-passed' }
} catch {
    $result.status = 'failed'
    $result.failure = $_.Exception.Message
    throw
} finally {
    Write-Json $result (Join-Path $out 'result.json')
    if ($lock) { Remove-Item -LiteralPath $lock -ErrorAction Stop }
}
Write-Host "Automated checks finished. LCD/keys/audio and performance interpretation remain manual. Logs: $out"
