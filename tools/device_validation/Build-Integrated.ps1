#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repo = 'C:\devs\m5stack\cardputer-adv-pocketjs',
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
    [switch]$Plan
)
. (Join-Path $PSScriptRoot 'Common.ps1')

function Assert-IntegratedConfiguration {
    param([string]$Build)
    $config = Join-Path $Build 'sdkconfig'
    if (!(Select-String -LiteralPath $config -Pattern '^CONFIG_IDF_TARGET="esp32s3"$' -Quiet)) {
        throw 'Generated config target mismatch.'
    }
    if (Select-String -LiteralPath $config -Pattern '^CONFIG_(POCKET_VM_(SELFTEST|OOMPROBE|PROBE|RELOC|TURNPERF)|KSN_DEVICE_PROBE)=y$' -Quiet) {
        throw 'Unexpected diagnostic config. This integrated build must be an ordinary image.'
    }
    $cache = Join-Path $Build 'CMakeCache.txt'
    if (!(Select-String -LiteralPath $cache -Pattern '^POCKET_APP_PRECOMPILE_EXPERIMENT:BOOL=OFF$' -Quiet)) {
        throw 'F must remain OFF in the generated CMake cache.'
    }
    # Check the real translation unit rather than trusting the requested flag.
    $compile = Get-Content -Raw -LiteralPath (Join-Path $Build 'compile_commands.json') | ConvertFrom-Json
    $qjs = @($compile | Where-Object { $_.file -match '[/\\]quickjs\.c$' })
    if ($qjs.Count -ne 1) { throw 'Expected exactly one QuickJS compile command.' }
    $flags = [regex]::Matches($qjs[0].command, '(?:^|\s)-DPOCKET_VM_TYPED_PUT_INT_FAST(?:=(\S+))?(?=\s|$)')
    if ($flags.Count -ne 1 -or $flags[0].Groups[1].Value -ne '0') {
        throw 'H OFF was not verified in the QuickJS compile command.'
    }
    $bytecode = Join-Path $Build 'experimental-bytecode'
    if ((Test-Path -LiteralPath $bytecode) -and @(Get-ChildItem -LiteralPath $bytecode -Recurse -File -Filter '*.bc').Count -ne 0) {
        throw 'Unexpected F bytecode outputs in the integrated build.'
    }
}

function Assert-PlainPath {
    param([string]$Path)
    # A junction could make a lexically external RunRoot point into a busy tree.
    # Refuse aliases rather than making promises about Windows path resolution.
    $cursor = $Path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Use a plain local path without symlinks or junctions: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}

Get-Command git -ErrorAction Stop | Out-Null
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$Repo = [IO.Path]::GetFullPath((Get-GitText $Repo @('rev-parse','--show-toplevel')))
$RunRoot = [IO.Path]::GetFullPath($RunRoot)
Assert-PlainPath $Repo
Assert-PlainPath $RunRoot
if ($RunRoot.StartsWith($Repo.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or $RunRoot.TrimEnd('\','/') -eq $Repo.TrimEnd('\','/')) {
    throw 'RunRoot must be outside the existing repository; use a fresh sibling directory.'
}
if (Test-Path -LiteralPath $RunRoot) {
    throw 'RunRoot already exists. Keep it; choose a new directory. Nothing is cleaned automatically.'
}
$Commit = $Commit.ToLowerInvariant()
$resolved = Get-GitText $Repo @('rev-parse','--verify',"$Commit^{commit}")
if ($resolved -ne $Commit) { throw 'Commit must resolve to the exact requested full commit SHA.' }
$worktree = Join-Path $RunRoot 'wt-integrated'
$build = Join-Path $worktree 'build_integrated'
$options = @(
    "-DSDKCONFIG=$build/sdkconfig",
    "-DSDKCONFIG_DEFAULTS=$worktree/sdkconfig.defaults",
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=0',
    '-DPOCKET_APP_PRECOMPILE_EXPERIMENT=OFF'
)
$entry = [ordered]@{
    role='integrated'; sha=$Commit; worktree=$worktree; build=$build
    cmakeOptions=$options; status='planned'; configurationVerified=$false
}
$manifest = [ordered]@{
    # The comparison's schema-1 reader must reject this even after a good build.
    schema='pocketjs-integrated-build-v1'; manifestType='integrated-build-only'
    status='planned'; createdUtc=[DateTime]::UtcNow.ToString('o')
    sourceRepository=$Repo; commit=$Commit; branchLabel='codex/vm-improvements'
    typedPutEnabled=$false; precompileExperimentEnabled=$false; runtimePrecompileIntegrated=$false
    builds=@($entry); deviceStatus='not run'; physicalChecks='not run'; failure=$null
    scripts=@(
        [ordered]@{name='Build-Integrated.ps1'; sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $PSCommandPath).Hash},
        [ordered]@{name='Common.ps1'; sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PSScriptRoot 'Common.ps1')).Hash}
    )
}
if ($Plan) { $manifest | ConvertTo-Json -Depth 12; return }
if ($env:IDF_TARGET -and $env:IDF_TARGET -ne 'esp32s3') { throw 'IDF_TARGET must be esp32s3 or unset.' }
foreach ($name in @('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','SDKCONFIG','SDKCONFIG_DEFAULTS')) {
    if ([Environment]::GetEnvironmentVariable($name)) { throw "Unset inherited $name before this controlled build." }
}
$python = Get-IdfPython
$idf = Join-Path $env:IDF_PATH 'tools/idf.py'
foreach ($tool in @('node','cmake','ninja','xtensa-esp32s3-elf-gcc')) { Get-Command $tool -ErrorAction Stop | Out-Null }
$nodeVersion = & node --version
if ($LASTEXITCODE -ne 0 -or $nodeVersion -notmatch '^v(\d+)\.' -or [int]$Matches[1] -lt 16) { throw 'Node >=16 is required.' }
New-Item -ItemType Directory -Path $RunRoot -ErrorAction Stop | Out-Null
$manifestPath = Join-Path $RunRoot 'manifest.json'
$manifest.status = 'building'
Write-Json $manifest $manifestPath
try {
    Invoke-Logged git @('--version') (Join-Path $RunRoot 'version-git.log')
    Invoke-Logged $python @('--version') (Join-Path $RunRoot 'version-python.log')
    Invoke-Logged $python @($idf,'--version') (Join-Path $RunRoot 'version-idf.log')
    foreach ($tool in @('node','cmake','ninja','xtensa-esp32s3-elf-gcc')) {
        Invoke-Logged $tool @('--version') (Join-Path $RunRoot "version-$tool.log")
    }
    Invoke-Logged git @('-C',$Repo,'worktree','add','--detach',$worktree,$Commit) (Join-Path $RunRoot 'integrated-worktree.log')
    if ((Get-GitText $worktree @('rev-parse','HEAD')) -ne $Commit) { throw 'Worktree HEAD mismatch.' }
    Push-Location $worktree
    try {
        Invoke-Logged $python @('tools/prepare_dependencies.py') (Join-Path $RunRoot 'integrated-dependencies.log')
        # Configure separately so diagnostic or unexpected experimental settings
        # stop the run before firmware compilation, then check them again after.
        $entry.status = 'configuring'
        Write-Json $manifest $manifestPath
        Invoke-Logged $python (@($idf,'-B',$build) + $options + @('reconfigure')) (Join-Path $RunRoot 'integrated-configure.log')
        Assert-IntegratedConfiguration $build
        $entry.configurationVerified = $true
        $entry.status = 'building'
        Write-Json $manifest $manifestPath
        Invoke-Logged $python (@($idf,'-B',$build) + $options + @('build')) (Join-Path $RunRoot 'integrated-build.log')
        Assert-IntegratedConfiguration $build
        Invoke-Logged $python @($idf,'-B',$build,'size') (Join-Path $RunRoot 'integrated-size.log')
        Invoke-Logged $python @($idf,'-B',$build,'size-components') (Join-Path $RunRoot 'integrated-size-components.log')
        $binary = Join-Path $build 'cardputer_pocketjs.bin'
        Invoke-Logged $python @('tools/check_flash.py',$binary) (Join-Path $RunRoot 'integrated-flash-budget.log')
        $entry.artifacts = @()
        foreach ($name in @('cardputer_pocketjs.bin','cardputer_pocketjs.elf','cardputer_pocketjs.map','sdkconfig','CMakeCache.txt','compile_commands.json')) {
            $path = Join-Path $build $name
            $entry.artifacts += [ordered]@{
                name=$name; path=$path; bytes=(Get-Item -LiteralPath $path -ErrorAction Stop).Length
                sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
            }
        }
        $entry.binary = $binary
        $entry.binaryBytes = (Get-Item -LiteralPath $binary).Length
        $entry.binarySha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash
        $entry.sdkconfigSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $build 'sdkconfig')).Hash
        # IDF may refresh the tracked dependency lock in a fresh checkout. Keep
        # that resolved input and its hash, without accepting other source edits.
        $lock = Join-Path $worktree 'dependencies.lock'
        if (Test-Path -LiteralPath $lock -PathType Leaf) {
            $entry.dependencyLock = [ordered]@{path=$lock; sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $lock).Hash}
        }
        if ((Get-GitText $worktree @('rev-parse','HEAD')) -ne $Commit) { throw 'Worktree HEAD changed during the build.' }
        $status = Get-GitText $worktree @('status','--porcelain')
        $status | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $RunRoot 'integrated-worktree-status.log')
        if (Get-GitText $worktree @('status','--porcelain','--','.',':(exclude)dependencies.lock')) {
            throw 'Source files changed during the build; inspect integrated-worktree-status.log.'
        }
        $entry.status = 'built'
    } finally { Pop-Location }
    $manifest.status = 'built'
} catch {
    $entry.status = 'failed'
    $manifest.status = 'failed'
    $manifest.failure = $_.Exception.Message
    throw
} finally { Write-Json $manifest $manifestPath }
Write-Host "Built integrated commit $Commit with H/F OFF. Manifest: $manifestPath. No serial access or flashing performed."
