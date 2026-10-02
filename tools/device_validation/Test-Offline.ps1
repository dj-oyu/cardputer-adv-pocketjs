#requires -Version 5.1
[CmdletBinding()]
param([string]$Python = 'python', [string]$Repo)
$ErrorActionPreference = 'Stop'
foreach ($file in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1') {
    $tokens = $null
    $parseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -gt 0) { throw ($parseErrors | Out-String) }
}
. (Join-Path $PSScriptRoot 'Common.ps1')
& (Join-Path $PSScriptRoot 'Test-PythonResolution.ps1')
# Resolve once so the suites and native fixture always use the same executable.
$pythonExe = Resolve-PythonApplication $Python
& $pythonExe (Join-Path $PSScriptRoot 'test_safe_smoke.py')
if ($LASTEXITCODE -ne 0) { throw 'Offline Python tests failed.' }
& $pythonExe (Join-Path $PSScriptRoot 'test_integrated_build.py')
if ($LASTEXITCODE -ne 0) { throw 'Integrated-build static guards failed.' }
& $pythonExe (Join-Path $PSScriptRoot 'test_quickjs_flags.py')
if ($LASTEXITCODE -ne 0) { throw 'QuickJS response-file flag tests failed.' }
function Test-LoggedNativeExit {
    param([string]$NativeExe)
    # Deliberately shadow the automatic variable in the caller. Invoke-Logged
    # must read the native process result, not this local value or a stale one.
    $LASTEXITCODE = 123
    $global:LASTEXITCODE = 97
    $ErrorActionPreference = 'Stop'
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('pocketjs-native-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -ErrorAction Stop | Out-Null
    try {
        $attempt = 0
        foreach ($expected in @(0, 7, 0)) {
            $log = Join-Path $temp ("native-{0}.log" -f $attempt)
            $attempt++
            # No embedded string quotes: compatible with legacy native argument passing.
            $nativeArgs = @('-c', 'import sys; print(12345); print(67890, file=sys.stderr); sys.exit(int(sys.argv[1]))', [string]$expected)
            $failure = $null
            $result = @()
            try { $result = @(Invoke-Logged $nativeExe $nativeArgs $log) }
            catch { $failure = $_.Exception.Message }
            if ($expected -eq 0 -and $null -ne $failure) { throw "Native exit-0 regression: $failure" }
            if ($expected -eq 7 -and ($null -eq $failure -or $failure -notmatch '^Command failed \(7\):')) {
                throw "Native exit-7 regression: expected numeric failure, got '$failure'"
            }
            if ($result.Count -ne 0) { throw 'Invoke-Logged leaked output into the success stream.' }
            $logged = Get-Content -Raw -LiteralPath $log
            if ($logged -notmatch '12345' -or $logged -notmatch '67890') { throw 'Native stdout/stderr log capture failed.' }
            if ($ErrorActionPreference -ne 'Stop' -or $LASTEXITCODE -ne 123) { throw 'Invoke-Logged changed caller-local state.' }
            if ($global:LASTEXITCODE -ne $expected) { throw 'Unexpected native process exit code.' }
        }
    } finally { Remove-Item -LiteralPath $temp -Recurse -Force }
}
Test-LoggedNativeExit $pythonExe
# A successful integrated build is still not a device-comparison manifest.
# Exercise both consumers with their opt-ins, stopping before any tool or port.
$isolated = Join-Path ([IO.Path]::GetTempPath()) ('pocketjs-integrated-schema-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $isolated -ErrorAction Stop | Out-Null
try {
    Write-Json ([ordered]@{schema='pocketjs-integrated-build-v1'; manifestType='integrated-build-only'; status='built'}) (Join-Path $isolated 'manifest.json')
    foreach ($script in @('Flash-Validation.ps1','Test-Device.ps1')) {
        $blocked = $false
        $params = @{RunRoot=$isolated; Role='candidate'; Port='COM9999'}
        if ($script -eq 'Flash-Validation.ps1') {
            $params.RecoveryImage = 'not-an-image.bin'
            $params.AllowFlash = $true
            $params.ConfirmCompatibleBootloaderAndPartitions = $true
        } else {
            $params.AllowDevice = $true
            $params.ConfirmRunningImage = $true
        }
        try { & (Join-Path $PSScriptRoot $script) @params }
        catch { $blocked = $_.Exception.Message -match 'successfully built schema-1 manifest' }
        if (!$blocked) { throw "Integrated-manifest exclusion failed: $script" }
    }
} finally { Remove-Item -LiteralPath $isolated -Recurse -Force }
foreach ($script in @('Flash-Validation.ps1','Test-Device.ps1')) {
    $blocked = $false
    try {
        $params = @{RunRoot='not-a-run'; Role='baseline'; Port='COM9999'}
        if ($script -eq 'Flash-Validation.ps1') { $params.RecoveryImage = 'not-an-image.bin' }
        & (Join-Path $PSScriptRoot $script) @params
    } catch { $blocked = $_.Exception.Message -match 'required' }
    if (!$blocked) { throw "Missing-opt-in guard test failed: $script" }
}
if ($Repo) {
    $unused = Join-Path ([IO.Path]::GetTempPath()) ('pocketjs-plan-' + [guid]::NewGuid().ToString('N'))
    $text = & (Join-Path $PSScriptRoot 'Build-Validation.ps1') -Repo $Repo -RunRoot $unused -Plan
    $plan = ($text -join "`n") | ConvertFrom-Json
    if ($plan.status -ne 'planned' -or $plan.builds.Count -ne 2 -or (Test-Path -LiteralPath $unused)) { throw 'Read-only plan test failed.' }
    $commit = Get-GitText $Repo @('rev-parse','HEAD')
    $text = & (Join-Path $PSScriptRoot 'Build-Integrated.ps1') -Repo $Repo -RunRoot $unused -Commit $commit -Plan
    $plan = ($text -join "`n") | ConvertFrom-Json
    if ($plan.schema -ne 'pocketjs-integrated-build-v1' -or $plan.manifestType -ne 'integrated-build-only' -or $plan.status -ne 'planned' -or $plan.builds.Count -ne 1 -or $plan.commit -ne $commit -or $plan.typedPutEnabled -or $plan.precompileExperimentEnabled -or $plan.runtimePrecompileIntegrated -or (Test-Path -LiteralPath $unused)) {
        throw 'Read-only integrated plan test failed.'
    }
    foreach ($invalid in @('HEAD','abc1234',($commit + '0'))) {
        $blocked = $false
        try { & (Join-Path $PSScriptRoot 'Build-Integrated.ps1') -Repo $Repo -RunRoot $unused -Commit $invalid -Plan | Out-Null }
        catch { $blocked = $_ -is [System.Management.Automation.ErrorRecord] -and $_.FullyQualifiedErrorId -match 'ParameterArgumentValidationError' }
        if (!$blocked -or (Test-Path -LiteralPath $unused)) { throw 'Integrated full-SHA guard test failed.' }
    }
}
Write-Host 'PowerShell parse, Python resolution, native exit-code/logging, and offline mock checks passed. No device or firmware build was tested.'
