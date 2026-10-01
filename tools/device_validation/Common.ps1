Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# PowerShell 7.3+ native stderr is still log output, not our failure signal.
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Invoke-Logged {
    param([string]$Exe, [string[]]$Arguments, [string]$Log)
    Get-Command $Exe -ErrorAction Stop | Out-Null
    $LASTEXITCODE = $null
    $old = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Exe @Arguments 2>&1 | Tee-Object -FilePath $Log | Out-Host
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    if ($null -eq $code -or $code -ne 0) { throw "Command failed ($code): $Exe; see $Log" }
}
function Get-GitText {
    param([string]$Repo, [string[]]$Arguments)
    $output = & git -C $Repo @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git failed: $($output -join ' ')" }
    return ($output -join "`n").Trim()
}
function Write-Json {
    param($Value, [string]$Path)
    $Value | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $Path
}
function Get-IdfPython {
    if (!$env:IDF_PATH -or !$env:IDF_PYTHON_ENV_PATH) {
        throw 'Activate the EIM ESP-IDF v6.0.1 PowerShell profile first.'
    }
    $python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
    $idf = Join-Path $env:IDF_PATH 'tools/idf.py'
    if (!(Test-Path -LiteralPath $python -PathType Leaf) -or !(Test-Path -LiteralPath $idf -PathType Leaf)) {
        throw 'EIM IDF Python or idf.py is missing; no fallback Python will be used.'
    }
    $version = & $python $idf --version 2>&1
    if ($LASTEXITCODE -ne 0 -or ($version -join ' ') -notmatch 'ESP-IDF v6\.0\.1(?:\s|$)') {
        throw "Expected ESP-IDF v6.0.1, found: $version"
    }
    return $python
}
function Read-Run {
    param([string]$RunRoot)
    $run = Get-Content -Raw -LiteralPath (Join-Path $RunRoot 'manifest.json') | ConvertFrom-Json
    if ($run.schema -ne 1 -or $run.status -ne 'built') { throw 'A successfully built schema-1 manifest is required.' }
    return $run
}
function Get-Artifact {
    param($Run, [string]$Role)
    $entry = @($Run.builds | Where-Object { $_.role -eq $Role })
    if ($entry.Count -ne 1 -or $entry[0].status -ne 'built') { throw 'The selected build is not complete.' }
    $entry = $entry[0]
    if (!(Test-Path -LiteralPath $entry.binary -PathType Leaf)) { throw 'Built image is missing.' }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $entry.binary).Hash -ne $entry.binarySha256) {
        throw 'Built image changed after manifest creation.'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $entry.build 'sdkconfig')).Hash -ne $entry.sdkconfigSha256) { throw 'Build sdkconfig changed after manifest creation.' }
    if ((Get-Item -LiteralPath $entry.binary).Length -gt 0x300000) { throw 'Image exceeds the factory app partition.' }
    return $entry
}
function Enter-DeviceLock {
    param([string]$Port)
    if (!$env:TEMP) { throw 'TEMP is unavailable; cannot acquire the shared serial lock.' }
    $path = Join-Path $env:TEMP "pocketjs-device-$($Port.ToUpperInvariant()).lock"
    # New-Item without -Force is atomic and refuses an existing lock.
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}
function New-StageDirectory {
    param([string]$Root, [string]$Stage)
    $path = Join-Path $Root ("{0}-{1}-{2}" -f $Stage, (Get-Date -Format 'yyyyMMdd-HHmmss'), [guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}
