Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'Common.ps1')

function Enter-IntegratedRunLock {
    param([string]$RunRoot)
    # Serialize this run's flash record even if two invocations name other ports.
    $path = Join-Path $RunRoot '.integrated-device.lock'
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}

function Read-IntegratedEvidence {
    param([string]$RunRoot, [string]$Commit, [string]$Python, [string]$Log)
    Invoke-Logged $Python @((Join-Path $PSScriptRoot 'verify_integrated_run.py'),
        '--run-root',$RunRoot,'--commit',$Commit) $Log
    $entry = Get-Content -Raw -LiteralPath $Log | ConvertFrom-Json
    if ($entry.status -ne 'preflight-verified' -or $entry.role -ne 'integrated' -or $entry.sourceSha -ne $Commit) {
        throw 'Integrated artifact preflight returned unexpected evidence.'
    }
    return $entry
}

function Assert-IntegratedFlashRecord {
    param($Flash, $Entry, [string]$Port)
    if ($Flash.schema -ne 'pocketjs-integrated-flash-v1' -or
        $Flash.status -ne 'flash-verified-awaiting-manual-reboot' -or
        $Flash.role -ne 'integrated' -or $Flash.port -ne $Port -or
        $Flash.sourceSha -ne $Entry.sourceSha -or
        $Flash.binarySha256 -ne $Entry.binarySha256 -or
        $Flash.manifestSha256 -ne $Entry.manifestSha256 -or
        $Flash.runRoot -ne $Entry.runRoot -or !$Flash.flashId) {
        throw 'No matching integrated flash verification. Run Flash-Integrated.ps1 for this exact commit, run and port first.'
    }
}
