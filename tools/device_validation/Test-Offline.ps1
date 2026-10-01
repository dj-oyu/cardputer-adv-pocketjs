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
& $Python (Join-Path $PSScriptRoot 'test_safe_smoke.py')
if ($LASTEXITCODE -ne 0) { throw 'Offline Python tests failed.' }
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
}
Write-Host 'PowerShell parse and offline mock checks passed. No device or firmware build was tested.'
