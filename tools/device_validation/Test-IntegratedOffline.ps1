#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PythonExe)
$ErrorActionPreference = 'Stop'
foreach ($suite in @('test_integrated_run.py','test_integrated_smoke.py')) {
    & $PythonExe (Join-Path $PSScriptRoot $suite)
    if ($LASTEXITCODE -ne 0) { throw "Integrated offline Python suite failed: $suite" }
}
# Run the real PowerShell orchestration against an isolated Common.ps1 mock.
# No IDF, pyserial, esptool or device process can be invoked by this fixture.
$root = Join-Path ([IO.Path]::GetTempPath()) ('pocketjs-integrated-mock-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
if (Test-Path variable:global:PocketIntegratedMock) { throw 'Offline fixture state already exists.' }
try {
    foreach ($name in @('IntegratedCommon.ps1','Flash-Integrated.ps1','Test-Integrated.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $root $name)
    }
    @'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Get-IdfPython { $global:PocketIntegratedMock.ToolChecks++; return 'mock-python-only' }
function Write-Json { param($Value,[string]$Path); $Value | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $Path }
function New-StageDirectory {
    param([string]$Root,[string]$Stage)
    $global:PocketIntegratedMock.Stages++
    $path = Join-Path $Root ($Stage + '-' + $global:PocketIntegratedMock.Stages)
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}
function Enter-DeviceLock {
    param([string]$Port)
    $path = Join-Path $global:PocketIntegratedMock.Root 'mock-port.lock'
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}
function Invoke-Logged {
    param([string]$Exe,[string[]]$Arguments,[string]$Log)
    if ($Exe -ne 'mock-python-only') { throw 'An unexpected executable reached the mock.' }
    $state = $global:PocketIntegratedMock
    $state.Commands.Add(($Arguments -join '|')) | Out-Null
    if ($Arguments[0] -like '*verify_integrated_run.py') {
        if ($state.RejectPreflight) { throw 'mock artifact mismatch' }
        Write-Json $state.Entry $Log
    } elseif ($Arguments[0] -like '*verify_port_identity.py') {
        if ($state.RejectPort) { throw 'mock COM identity changed' }
        'identity matched' | Set-Content -LiteralPath $Log
    } elseif ($Arguments -contains 'verify-flash') {
        if ($state.FailVerify) { throw 'mock verify failure' }
        'verified' | Set-Content -LiteralPath $Log
    } elseif ($Arguments -contains 'image-info') {
        'Chip ID: ESP32-S3' | Set-Content -LiteralPath $Log
    } elseif ($Arguments[0] -like '*integrated_smoke.py') {
        if ($state.FailSmoke) { throw 'mock smoke failure' }
        'ordinary mock passed' | Set-Content -LiteralPath $Log
    } else {
        'mock log only' | Set-Content -LiteralPath $Log
    }
}
'@ | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'Common.ps1')
    $binary = Join-Path $root 'fixture-app.bin'
    [IO.File]::WriteAllBytes($binary, [byte[]](1,2,3,4))
    $commit = '2222222222222222222222222222222222222222'
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash
    $global:PocketIntegratedMock = @{
        Root=$root; ToolChecks=0; Stages=0; RejectPreflight=$false; FailVerify=$false; FailSmoke=$false; RejectPort=$false
        Commands=(New-Object 'System.Collections.Generic.List[string]')
        Entry=[pscustomobject]@{status='preflight-verified'; role='integrated'; sourceSha=$commit;
            binary=$binary; binarySha256=$hash; manifestSha256=('a' * 64); runRoot=$root}
    }
    $flashParams = @{RunRoot=$root; Commit=$commit; Port='COM9999'; RecoveryImage=$binary}
    $testParams = @{RunRoot=$root; Commit=$commit; Port='COM9999'}
    foreach ($script in @('Flash-Integrated.ps1','Test-Integrated.ps1')) {
        $blocked = $false
        try {
            if ($script -eq 'Flash-Integrated.ps1') { & (Join-Path $root $script) @flashParams }
            else { & (Join-Path $root $script) @testParams }
        } catch { $blocked = $_.Exception.Message -match 'Required:' }
        if (!$blocked -or $global:PocketIntegratedMock.ToolChecks -ne 0) { throw 'Integrated opt-in failed before tool access.' }
    }
    $flashParams.AllowFlash = $true
    $flashParams.ConfirmCompatibleBootloaderAndPartitions = $true
    $flashParams.ConfirmDeviceFree = $true
    $flashParams.ConfirmDownloadMode = $true
    $testParams.AllowDevice = $true
    $testParams.ConfirmDeviceFree = $true
    $testParams.ConfirmRunningImage = $true
    $testParams.Grid = $true
    # GUI plan pins fail before write-flash, including a changed recovery source.
    foreach ($name in @('ExpectedBinarySha256','ExpectedManifestSha256','ExpectedRecoverySha256')) {
        $bad = $flashParams.Clone()
        $bad[$name] = ('f' * 64)
        $beforeWrites = @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match '\|write-flash\|' }).Count
        $blocked = $false
        try { & (Join-Path $root 'Flash-Integrated.ps1') @bad }
        catch { $blocked = $_.Exception.Message -match 'differs from the approved plan' }
        if (!$blocked -or @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match '\|write-flash\|' }).Count -ne $beforeWrites) {
            throw "GUI pin did not stop a write: $name"
        }
    }
    $flashParams.ExpectedBinarySha256 = $hash
    $flashParams.ExpectedManifestSha256 = ('a' * 64)
    $flashParams.ExpectedRecoverySha256 = $hash
    $flashParams.ExpectedPortSignature = ('c' * 64)
    $testParams.ExpectedBinarySha256 = $hash
    $testParams.ExpectedManifestSha256 = ('a' * 64)
    $testParams.ExpectedPortSignature = ('c' * 64)
    & (Join-Path $root 'Flash-Integrated.ps1') @flashParams
    $recordPath = Join-Path $root 'last-integrated-flash.json'
    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    if ($record.status -ne 'flash-verified-awaiting-manual-reboot' -or $record.sourceSha -ne $commit) { throw 'Successful flash record mismatch.' }
    if ($record.recoveryConfidence -ne 'user-attested-known-good' -or
        (Split-Path -Leaf $record.recoveryImage) -ne 'known-good-app.bin') { throw 'Default recovery attestation was not recorded.' }
    $writes = @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match '\|write-flash\|' })
    if ($writes.Count -ne 1 -or $writes[0] -notmatch '\|--before\|no-reset\|--after\|no-reset\|--no-stub\|write-flash\|0x10000\|') { throw 'App-only/no-reset command changed.' }
    $flashParams.AllowUnverifiedRecovery = $true
    $recoveryWarnings = @()
    & (Join-Path $root 'Flash-Integrated.ps1') @flashParams -WarningVariable recoveryWarnings -WarningAction SilentlyContinue
    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    if ($record.status -ne 'flash-verified-awaiting-manual-reboot' -or
        $record.recoveryConfidence -ne 'unverified-candidate' -or
        (Split-Path -Leaf $record.recoveryImage) -ne 'unverified-recovery-app.bin' -or
        $record.recoverySha256 -ne $hash -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $record.recoveryImage).Hash -ne $hash -or
        ($recoveryWarnings -join ' ') -notmatch 'no verified rollback guarantee') {
        throw 'Unverified recovery confidence, warning or retained hash mismatch.'
    }
    & (Join-Path $root 'Test-Integrated.ps1') @testParams
    if (!@($global:PocketIntegratedMock.Commands | Where-Object { $_ -match 'integrated_smoke.py.*\|--grid$' }).Count) { throw 'Ordinary GRID runner was not selected.' }
    $badTest = $testParams.Clone()
    $badTest.ExpectedFlashId = ('f' * 32)
    $beforeTests = @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match 'integrated_smoke.py' }).Count
    $blocked = $false
    try { & (Join-Path $root 'Test-Integrated.ps1') @badTest }
    catch { $blocked = $_.Exception.Message -match 'differs from the approved plan' }
    if (!$blocked -or @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match 'integrated_smoke.py' }).Count -ne $beforeTests) {
        throw 'GUI flash ID pin did not stop device tests.'
    }
    $global:PocketIntegratedMock.RejectPort = $true
    $beforeWrites = @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match '\|write-flash\|' }).Count
    $blocked = $false
    try { & (Join-Path $root 'Flash-Integrated.ps1') @flashParams }
    catch { $blocked = $_.Exception.Message -match 'mock COM identity changed' }
    if (!$blocked -or @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match '\|write-flash\|' }).Count -ne $beforeWrites) {
        throw 'Changed COM identity reached write-flash.'
    }
    $global:PocketIntegratedMock.RejectPort = $false
    $record.port = 'COM9998'
    $record | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $recordPath
    $count = @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match 'integrated_smoke.py' }).Count
    $blocked = $false
    try { & (Join-Path $root 'Test-Integrated.ps1') @testParams }
    catch { $blocked = $_.Exception.Message -match 'No matching integrated flash' }
    if (!$blocked -or @($global:PocketIntegratedMock.Commands | Where-Object { $_ -match 'integrated_smoke.py' }).Count -ne $count) { throw 'Mismatched flash record allowed a runner.' }
    $global:PocketIntegratedMock.FailVerify = $true
    $blocked = $false
    try { & (Join-Path $root 'Flash-Integrated.ps1') @flashParams }
    catch { $blocked = $_.Exception.Message -match 'mock verify failure' }
    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    if (!$blocked -or $record.status -ne 'failed') { throw 'Failed verification preserved stale success.' }
    foreach ($lock in @('.integrated-device.lock','mock-port.lock')) {
        if (Test-Path -LiteralPath (Join-Path $root $lock)) { throw "Failure leaked lock: $lock" }
    }
    $global:PocketIntegratedMock.RejectPreflight = $true
    $before = $global:PocketIntegratedMock.Commands.Count
    $blocked = $false
    try { & (Join-Path $root 'Flash-Integrated.ps1') @flashParams }
    catch { $blocked = $_.Exception.Message -match 'mock artifact mismatch' }
    if (!$blocked -or $global:PocketIntegratedMock.Commands.Count -ne $before + 1) { throw 'Rejected artifact reached a device command.' }
    $global:PocketIntegratedMock.RejectPreflight = $false
    $lockPath = Join-Path $root '.integrated-device.lock'
    New-Item -ItemType Directory -Path $lockPath | Out-Null
    $before = $global:PocketIntegratedMock.Commands.Count
    $blocked = $false
    try { & (Join-Path $root 'Flash-Integrated.ps1') @flashParams }
    catch { $blocked = $true }
    if (!$blocked -or !(Test-Path -LiteralPath $lockPath) -or $global:PocketIntegratedMock.Commands.Count -ne $before) { throw 'Busy same-run lock was bypassed or removed.' }
    Remove-Item -LiteralPath $lockPath
} finally {
    Remove-Variable -Name PocketIntegratedMock -Scope Global -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -Recurse -Force
}
Write-Host 'Integrated PowerShell orchestration mocks passed. No device, serial process or firmware build was run.'
