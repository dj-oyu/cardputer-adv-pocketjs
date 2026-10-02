#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [Parameter(Mandatory=$true)][ValidateSet('baseline','candidate')][string]$Role,
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [Parameter(Mandatory=$true)][string]$RecoveryImage,
    [switch]$AllowFlash,
    [switch]$ConfirmCompatibleBootloaderAndPartitions
)
. (Join-Path $PSScriptRoot 'Common.ps1')
# Check all acknowledgements before even constructing a serial command.
if (!$AllowFlash -or !$ConfirmCompatibleBootloaderAndPartitions) {
    throw 'Review README: this replaces only the app partition. Both -AllowFlash and -ConfirmCompatibleBootloaderAndPartitions are required.'
}
$RunRoot = (Resolve-Path -LiteralPath $RunRoot).Path
$run = Read-Run $RunRoot
if ($Role -eq 'candidate' -and $run.candidate -eq 'F') { throw 'F is a build-only experiment; this harness does not flash it.' }
$entry = Get-Artifact $run $Role
$RecoveryImage = (Resolve-Path -LiteralPath $RecoveryImage).Path
if (!(Test-Path -LiteralPath $RecoveryImage -PathType Leaf) -or (Get-Item -LiteralPath $RecoveryImage).Length -gt 0x300000) {
    throw 'Supply your known-good factory app binary, not a full-flash backup.'
}
$python = Get-IdfPython
$out = New-StageDirectory $RunRoot "flash-$Role"
$record = [ordered]@{role=$Role; port=$Port; binarySha256=$entry.binarySha256; sourceSha=$entry.sha; status='preflight'; runningImage='not verified'; startedUtc=[DateTime]::UtcNow.ToString('o'); failure=$null}
$lock = $null
try {
    Invoke-Logged $python @('-m','esptool','version') (Join-Path $out 'esptool-version.log')
    Invoke-Logged $python @('-m','esptool','image-info',$entry.binary) (Join-Path $out 'image-info.log')
    Invoke-Logged $python @('-m','esptool','image-info',$RecoveryImage) (Join-Path $out 'recovery-image-info.log')
    foreach ($log in @('image-info.log','recovery-image-info.log')) {
        if (!(Select-String -LiteralPath (Join-Path $out $log) -Pattern 'ESP32-S3' -Quiet)) { throw 'Image chip was not confirmed as ESP32-S3.' }
    }
    $recoveryCopy = Join-Path $out 'known-good-app.bin'
    Copy-Item -LiteralPath $RecoveryImage -Destination $recoveryCopy -ErrorAction Stop
    $record.recoveryImage = $recoveryCopy
    $record.recoverySha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $recoveryCopy).Hash
    $partition = Get-Content -Raw -LiteralPath (Join-Path $entry.worktree 'partitions.csv')
    if ($partition -notmatch '(?m)^factory,app,factory,0x10000,0x300000,') { throw 'Unexpected source partition layout.' }
    $lock = Enter-DeviceLock $Port
    Write-Host 'Device must already be in download mode. No automatic reset or restore will occur.'
    # no-reset on BOTH sides: entering download mode and rebooting are manual.
    # write-flash erases only sectors occupied by this app image, never erase-all.
    $esp = @('-m','esptool','--chip','esp32s3','--port',$Port,'--before','no-reset','--after','no-reset','--no-stub')
    $record.status = 'flashing'
    Write-Json $record (Join-Path $out 'result.json')
    Write-Json $record (Join-Path $RunRoot 'last-flash.json')
    Invoke-Logged $python ($esp + @('write-flash','0x10000',$entry.binary)) (Join-Path $out 'write.log')
    Invoke-Logged $python ($esp + @('verify-flash','0x10000',$entry.binary)) (Join-Path $out 'verify.log')
    $record.status = 'flash-verified-awaiting-manual-reboot'
    Write-Json $record (Join-Path $RunRoot 'last-flash.json')
} catch {
    $record.status = 'failed'
    $record.failure = $_.Exception.Message
    # Replace stale success state so a failed later flash cannot authorize tests.
    Write-Json $record (Join-Path $RunRoot 'last-flash.json')
    throw
} finally {
    Write-Json $record (Join-Path $out 'result.json')
    if ($lock) { Remove-Item -LiteralPath $lock -ErrorAction Stop }
}
Write-Host "Flash bytes verified. Manually reboot, wait for home, then run Test-Device.ps1. Logs: $out"
