#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repo = 'C:\devs\m5stack\cardputer-adv-pocketjs',
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [string]$Python = 'python',
    [switch]$Remove,
    [switch]$ConfirmNoActiveProcesses
)
. (Join-Path $PSScriptRoot 'Common.ps1')
if ($Remove -and !$ConfirmNoActiveProcesses) {
    throw '-Remove requires -ConfirmNoActiveProcesses after all builds, flashes, tests and terminals using this run have stopped.'
}
$pythonExe = Resolve-PythonApplication $Python
$arguments = @((Join-Path $PSScriptRoot 'cleanup_validation.py'), '--repo', $Repo, '--run-root', $RunRoot)
if ($Remove) { $arguments += '--remove' }
if ($ConfirmNoActiveProcesses) { $arguments += '--confirm-no-active-processes' }
& $pythonExe @arguments
if ($LASTEXITCODE -ne 0) { throw 'Cleanup refused or incomplete. See the report above; nothing is force-deleted.' }
