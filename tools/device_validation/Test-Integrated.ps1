#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedPortSignature,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedBinarySha256,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedManifestSha256,
    [ValidatePattern('^[0-9a-f]{32}$')][string]$ExpectedFlashId,
    [ValidateRange(1,20)][int]$Cycles = 3,
    [switch]$Grid,
    [switch]$AllowDevice,
    [switch]$ConfirmDeviceFree,
    [switch]$ConfirmRunningImage
)
. (Join-Path $PSScriptRoot 'IntegratedCommon.ps1')
if (!$AllowDevice -or !$ConfirmDeviceFree -or !$ConfirmRunningImage) {
    throw 'Required: -AllowDevice, -ConfirmDeviceFree and -ConfirmRunningImage. You must manually reboot the verified integrated image, wait for home and close all other serial users first.'
}
$RunRoot = (Resolve-Path -LiteralPath $RunRoot).Path
$Commit = $Commit.ToLowerInvariant()
$Port = $Port.ToUpperInvariant()
$python = Get-IdfPython
$out = New-StageDirectory $RunRoot 'device-integrated'
$runLock = $null
$portLock = $null
$result = [ordered]@{role='integrated'; sourceSha=$Commit; port=$Port; status='preflight';
    runningImage='user attested after verified flash'; physicalChecks='manual, not assessed';
    startedUtc=[DateTime]::UtcNow.ToString('o'); failure=$null}
try {
    $runLock = Enter-IntegratedRunLock $RunRoot
    $entry = Read-IntegratedEvidence $RunRoot $Commit $python (Join-Path $out 'preflight.json')
    $portLock = Enter-DeviceLock $Port
    # Read flash state only under both locks, not before waiting for their owners.
    $flash = Get-Content -Raw -LiteralPath (Join-Path $RunRoot 'last-integrated-flash.json') | ConvertFrom-Json
    Assert-IntegratedFlashRecord $flash $entry $Port
    if (($ExpectedBinarySha256 -and $entry.binarySha256 -ne $ExpectedBinarySha256) -or
        ($ExpectedManifestSha256 -and $entry.manifestSha256 -ne $ExpectedManifestSha256) -or
        ($ExpectedFlashId -and $flash.flashId -ne $ExpectedFlashId)) {
        throw 'Integrated flash evidence differs from the approved plan; prepare it again.'
    }
    $result.binarySha256 = $entry.binarySha256
    $result.manifestSha256 = $entry.manifestSha256
    $result.flashId = $flash.flashId
    Invoke-Logged $python @('-c','import serial; print(serial.__version__)') (Join-Path $out 'pyserial-version.log')
    Write-Host "USER CONTROL: keep $Port free. Testing role integrated; source $Commit; image $($entry.binarySha256). No reset or automatic recovery."
    $arguments = @((Join-Path $PSScriptRoot 'integrated_smoke.py'),'--allow-device','--port',$Port,
        '--cycles',"$Cycles",'--out',(Join-Path $out 'ordinary'))
    if ($Grid) { $arguments += '--grid' }
    Assert-ExpectedPortIdentity $Port $ExpectedPortSignature $python (Join-Path $out 'port-before-test.log')
    Invoke-Logged $python $arguments (Join-Path $out 'ordinary-console.log')
    $result.status = if ($Grid) { 'hello-and-grid-markers-passed' } else { 'hello-markers-passed' }
} catch {
    $result.status = 'failed'
    $result.failure = $_.Exception.Message
    throw
} finally {
    try { Write-Json $result (Join-Path $out 'result.json') }
    finally {
        try { if ($portLock) { Remove-Item -LiteralPath $portLock -ErrorAction Stop } }
        finally { if ($runLock) { Remove-Item -LiteralPath $runLock -ErrorAction Stop } }
    }
}
Write-Host "Ordinary markers passed. The device is free; LCD/physical keys/audio and performance interpretation remain yours to check. Logs: $out"
