#requires -Version 5.1
[CmdletBinding()]
param()

# The child scope keeps the command mock out of the caller's session. No Python
# installation, PATH changes, firmware tools or device access are required.
& {
    . (Join-Path $PSScriptRoot 'Common.ps1')
    function Get-Command {
        [CmdletBinding()]
        param([string]$Name, [string]$CommandType)
        if ($CommandType -ne 'Application' -or $PSBoundParameters['ErrorAction'] -ne 'Stop') {
            throw 'Python resolution must require an application and stop on lookup errors.'
        }
        switch -Exact ($Name) {
            'python' {
                [pscustomobject]@{Source='C:\first python\python.exe'}
                [pscustomobject]@{Source='C:\WindowsApps\python.exe'}
            }
            'C:\chosen python\python.exe' {
                [pscustomobject]@{Source=$Name}
            }
            default { throw 'Mock Python application not found.' }
        }
    }

    $resolved = @(Resolve-PythonApplication 'python')
    if ($resolved.Count -ne 1 -or $resolved[0] -isnot [string] -or $resolved[0] -cne 'C:\first python\python.exe') {
        throw 'Multiple-application regression: expected exactly the first executable path.'
    }
    $explicit = 'C:\chosen python\python.exe'
    $resolved = @(Resolve-PythonApplication $explicit)
    if ($resolved.Count -ne 1 -or $resolved[0] -isnot [string] -or $resolved[0] -cne $explicit) {
        throw 'Explicit-Python regression: the requested executable was not preserved.'
    }
    $blocked = $false
    try { Resolve-PythonApplication 'missing-python' | Out-Null }
    catch { $blocked = $_.Exception.Message -eq 'Mock Python application not found.' }
    if (!$blocked) { throw 'Missing-Python regression: lookup failure must stop without a fallback.' }
}
Write-Host 'Python application resolution mock checks passed.'
