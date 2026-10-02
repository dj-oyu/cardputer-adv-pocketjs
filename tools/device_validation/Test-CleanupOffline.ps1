#requires -Version 5.1
[CmdletBinding()]
param([string]$Python = 'python')
. (Join-Path $PSScriptRoot 'Common.ps1')
foreach ($name in @('Cleanup-Validation.ps1', 'Test-CleanupOffline.ps1')) {
    $tokens = $null
    $parseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -gt 0) { throw ($parseErrors | Out-String) }
}
$pythonExe = Resolve-PythonApplication $Python
& $pythonExe (Join-Path $PSScriptRoot 'test_cleanup_validation.py')
if ($LASTEXITCODE -ne 0) { throw 'Offline cleanup mocks failed.' }
Write-Host 'Cleanup parser and offline Python mocks passed. No real worktree, hardware or firmware was tested.'
