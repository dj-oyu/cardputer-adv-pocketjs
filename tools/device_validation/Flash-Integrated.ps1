#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [Parameter(Mandatory=$true)][string]$RecoveryImage,
    [ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedPortSignature,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedBinarySha256,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedManifestSha256,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedRecoverySha256,
    [switch]$AllowUnverifiedRecovery,
    [switch]$AllowFlash,
    [switch]$ConfirmCompatibleBootloaderAndPartitions,
    [switch]$ConfirmDeviceFree,
    [switch]$ConfirmDownloadMode
)
. (Join-Path $PSScriptRoot 'IntegratedCommon.ps1')
if (!$AllowFlash -or !$ConfirmCompatibleBootloaderAndPartitions -or !$ConfirmDeviceFree -or !$ConfirmDownloadMode) {
    throw 'Required: -AllowFlash, -ConfirmCompatibleBootloaderAndPartitions, -ConfirmDeviceFree and -ConfirmDownloadMode. Read INTEGRATED-DEVICE.md; take control of the device yourself and enter download mode manually.'
}
$RunRoot = (Resolve-Path -LiteralPath $RunRoot).Path
$Commit = $Commit.ToLowerInvariant()
$Port = $Port.ToUpperInvariant()
$python = Get-IdfPython
$out = New-StageDirectory $RunRoot 'flash-integrated'
$runLock = $null
$portLock = $null
$record = $null
try {
    $runLock = Enter-IntegratedRunLock $RunRoot
    $entry = Read-IntegratedEvidence $RunRoot $Commit $python (Join-Path $out 'preflight.json')
    # Optional GUI plan pins are checked under the run lock, before any port use.
    if (($ExpectedBinarySha256 -and $entry.binarySha256 -ne $ExpectedBinarySha256) -or
        ($ExpectedManifestSha256 -and $entry.manifestSha256 -ne $ExpectedManifestSha256)) {
        throw 'Integrated evidence differs from the approved plan; prepare it again.'
    }
    $record = [ordered]@{
        schema='pocketjs-integrated-flash-v1'; role='integrated'; runRoot=$entry.runRoot
        flashId=[guid]::NewGuid().ToString('N'); port=$Port; sourceSha=$entry.sourceSha
        binarySha256=$entry.binarySha256; manifestSha256=$entry.manifestSha256
        recoveryConfidence=$(if ($AllowUnverifiedRecovery) { 'unverified-candidate' } else { 'user-attested-known-good' })
        status='preflight'; runningImage='not verified'; startedUtc=[DateTime]::UtcNow.ToString('o'); failure=$null
    }
    # Invalidate old authorization before starting any new flash attempt.
    Write-Json $record (Join-Path $RunRoot 'last-integrated-flash.json')
    $RecoveryImage = (Resolve-Path -LiteralPath $RecoveryImage).Path
    if (!(Test-Path -LiteralPath $RecoveryImage -PathType Leaf) -or
        (Get-Item -LiteralPath $RecoveryImage).Length -le 0 -or
        (Get-Item -LiteralPath $RecoveryImage).Length -gt 0x300000) {
        throw 'Supply a nonempty factory app binary within 0x300000 bytes, not a full-flash backup.'
    }
    if ($AllowUnverifiedRecovery) {
        Write-Warning 'Recovery image is an unverified candidate. Chip, size and hash checks do not prove it boots; there is no verified rollback guarantee.'
    }
    # Flash an exact private snapshot; never pass a mutable build output to esptool.
    $image = Join-Path $out 'integrated-app.bin'
    Copy-Item -LiteralPath $entry.binary -Destination $image -ErrorAction Stop
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash -ne $entry.binarySha256) {
        throw 'Built image changed while copying it; no device operation performed.'
    }
    $recoveryName = if ($AllowUnverifiedRecovery) { 'unverified-recovery-app.bin' } else { 'known-good-app.bin' }
    $recoveryCopy = Join-Path $out $recoveryName
    $recoveryHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $RecoveryImage).Hash
    if ($ExpectedRecoverySha256 -and $recoveryHash -ne $ExpectedRecoverySha256) {
        throw 'Recovery image differs from the approved plan; prepare it again.'
    }
    Copy-Item -LiteralPath $RecoveryImage -Destination $recoveryCopy -ErrorAction Stop
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $recoveryCopy).Hash -ne $recoveryHash) {
        throw 'Recovery image changed while copying it.'
    }
    $record.image = $image
    $record.recoveryImage = $recoveryCopy
    $record.recoverySha256 = $recoveryHash
    Invoke-Logged $python @('-m','esptool','version') (Join-Path $out 'esptool-version.log')
    Invoke-Logged $python @('-m','esptool','image-info',$image) (Join-Path $out 'image-info.log')
    Invoke-Logged $python @('-m','esptool','image-info',$recoveryCopy) (Join-Path $out 'recovery-image-info.log')
    foreach ($log in @('image-info.log','recovery-image-info.log')) {
        if (!(Select-String -LiteralPath (Join-Path $out $log) -Pattern 'ESP32-S3' -Quiet)) {
            throw 'Image chip was not confirmed as ESP32-S3.'
        }
    }
    $portLock = Enter-DeviceLock $Port
    # Check retained evidence again after acquiring both locks, before any port use.
    $current = Read-IntegratedEvidence $RunRoot $Commit $python (Join-Path $out 'preflight-locked.json')
    if ($current.manifestSha256 -ne $entry.manifestSha256 -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash -ne $entry.binarySha256) {
        throw 'Build evidence or staged image changed during preflight.'
    }
    Write-Host "USER CONTROL: $Port must be free and already in download mode. Role integrated; source $Commit; image $($entry.binarySha256)."
    Write-Host 'App partition only at 0x10000. No automatic reset, erase-all or restore.'
    $esp = @('-m','esptool','--chip','esp32s3','--port',$Port,'--before','no-reset','--after','no-reset','--no-stub')
    $record.status = 'flashing'
    Write-Json $record (Join-Path $out 'result.json')
    Write-Json $record (Join-Path $RunRoot 'last-integrated-flash.json')
    Assert-ExpectedPortIdentity $Port $ExpectedPortSignature $python (Join-Path $out 'port-before-write.log')
    Invoke-Logged $python ($esp + @('write-flash','0x10000',$image)) (Join-Path $out 'write.log')
    Assert-ExpectedPortIdentity $Port $ExpectedPortSignature $python (Join-Path $out 'port-before-verify.log')
    Invoke-Logged $python ($esp + @('verify-flash','0x10000',$image)) (Join-Path $out 'verify.log')
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash -ne $entry.binarySha256) {
        throw 'Staged image changed during flash verification.'
    }
    $record.status = 'flash-verified-awaiting-manual-reboot'
    Write-Json $record (Join-Path $RunRoot 'last-integrated-flash.json')
} catch {
    if ($record) {
        $record.status = 'failed'
        $record.failure = $_.Exception.Message
        Write-Json $record (Join-Path $RunRoot 'last-integrated-flash.json')
    }
    throw
} finally {
    try {
        if ($record) { Write-Json $record (Join-Path $out 'result.json') }
    } finally {
        try { if ($portLock) { Remove-Item -LiteralPath $portLock -ErrorAction Stop } }
        finally { if ($runLock) { Remove-Item -LiteralPath $runLock -ErrorAction Stop } }
    }
}
Write-Host "Flash bytes verified. The device is yours: manually reboot, wait for home, then use Test-Integrated.ps1. Logs: $out"
