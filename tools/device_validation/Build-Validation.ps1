#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repo = 'C:\devs\m5stack\cardputer-adv-pocketjs',
    [Parameter(Mandatory=$true)][string]$RunRoot,
    [ValidateSet('Y','A','C','E','F','H')][string]$Candidate = 'Y',
    [switch]$EnableTypedPut,
    [switch]$EnablePrecompileExperiment,
    [string]$HostCc,
    [switch]$Plan
)
. (Join-Path $PSScriptRoot 'Common.ps1')
$baseline = '89a71ae72c721206dfbb63acfeffc74b38dde604'
$candidates = @{
    Y = @('codex/y-scoped-grid-borrow', 'c5a4993f399e57c1df31054373f1c42cf62b9e57')
    A = @('vm/cloud-v2-a-frame-entry', '22e2e14f2ce4da33c4b1dabdc4375b16f81c6406')
    C = @('vm/cloud-v2-c-parser-shrink', 'ea72fbb90c8551f2e069c550b0f14a08bde31150')
    E = @('vm/cloud-v2-e-array-trim', 'c17d3c5cfee9996994f50bc4bdbc4b15e4058e75')
    F = @('vm/cloud-v2-f-precompile', '1e14ab786f7db671fd12cc7ffa779b3f8350d718')
    H = @('vm/cloud-v2-h-typed-put', '85798a73ccf75ef243dde3728fe163f58420e27a')
}
if ($EnableTypedPut -and $Candidate -ne 'H') { throw '-EnableTypedPut is only valid for H.' }
if ($EnablePrecompileExperiment -and $Candidate -ne 'F') { throw '-EnablePrecompileExperiment is only valid for F.' }
if ($HostCc -and !$EnablePrecompileExperiment) { throw '-HostCc requires -EnablePrecompileExperiment.' }
if ($EnablePrecompileExperiment -and (!$HostCc -or !(Test-Path -LiteralPath $HostCc -PathType Leaf))) {
    throw 'F enabled needs -HostCc pointing to a native Windows gcc/clang executable (not Xtensa/WSL).'
}
Get-Command git -ErrorAction Stop | Out-Null
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$RunRoot = [IO.Path]::GetFullPath($RunRoot)
if ($RunRoot.StartsWith($Repo.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or $RunRoot -eq $Repo) {
    throw 'RunRoot must be outside the existing repository; use a fresh sibling directory.'
}
if (Test-Path -LiteralPath $RunRoot) { throw 'RunRoot already exists. Keep it; choose a new directory. Nothing is cleaned automatically.' }
$selection = $candidates[$Candidate]
foreach ($sha in @($baseline, $selection[1])) {
    $resolved = Get-GitText $Repo @('rev-parse','--verify',"$sha^{commit}")
    if ($resolved -ne $sha) { throw "Missing exact commit $sha. Fetch origin/vm/main and origin/$($selection[0]) explicitly, then retry." }
}
$entries = @()
foreach ($role in @('baseline','candidate')) {
    $sha = if ($role -eq 'baseline') { $baseline } else { $selection[1] }
    $worktree = Join-Path $RunRoot "wt-$role"
    $build = Join-Path $worktree 'build_device_validation'
    $options = @("-DSDKCONFIG=$build/sdkconfig", "-DSDKCONFIG_DEFAULTS=$worktree/sdkconfig.defaults")
    if ($role -eq 'candidate' -and $Candidate -eq 'H') {
        $enabled = if ($EnableTypedPut) { 1 } else { 0 }
        $options += "-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=$enabled"
    }
    if ($role -eq 'candidate' -and $Candidate -eq 'F') {
        $flag = if ($EnablePrecompileExperiment) { 'ON' } else { 'OFF' }
        $options += "-DPOCKET_APP_PRECOMPILE_EXPERIMENT=$flag"
        if ($EnablePrecompileExperiment) { $options += "-DPOCKET_APP_HOST_CC=$HostCc" }
    }
    $entries += [ordered]@{role=$role; sha=$sha; worktree=$worktree; build=$build; cmakeOptions=$options; status='planned'}
}
$manifest = [ordered]@{
    schema=1; status='planned'; createdUtc=[DateTime]::UtcNow.ToString('o'); candidate=$Candidate
    sourceRepository=$Repo; branchLabel=$selection[0]; typedPutEnabled=[bool]$EnableTypedPut
    precompileExperimentEnabled=[bool]$EnablePrecompileExperiment; runtimePrecompileIntegrated=$false
    builds=$entries; deviceStatus='not run'; physicalChecks='not run'; failure=$null
}
if ($Plan) { $manifest | ConvertTo-Json -Depth 12; return }
if ($env:IDF_TARGET -and $env:IDF_TARGET -ne 'esp32s3') { throw 'IDF_TARGET must be esp32s3 or unset.' }
foreach ($name in @('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','SDKCONFIG_DEFAULTS')) {
    if ([Environment]::GetEnvironmentVariable($name)) { throw "Unset inherited $name before this controlled comparison." }
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
    if ($EnablePrecompileExperiment) { Invoke-Logged $HostCc @('--version') (Join-Path $RunRoot 'version-host-cc.log') }
    foreach ($entry in $entries) {
        $role = $entry.role
        Invoke-Logged git @('-C',$Repo,'worktree','add','--detach',$entry.worktree,$entry.sha) (Join-Path $RunRoot "$role-worktree.log")
        if ((Get-GitText $entry.worktree @('rev-parse','HEAD')) -ne $entry.sha) { throw 'Worktree HEAD mismatch.' }
        Push-Location $entry.worktree
        try {
            Invoke-Logged $python @('tools/prepare_dependencies.py') (Join-Path $RunRoot "$role-dependencies.log")
            Invoke-Logged $python (@($idf,'-B',$entry.build) + $entry.cmakeOptions + @('build')) (Join-Path $RunRoot "$role-build.log")
            Invoke-Logged $python @($idf,'-B',$entry.build,'size') (Join-Path $RunRoot "$role-size.log")
            Invoke-Logged $python @($idf,'-B',$entry.build,'size-components') (Join-Path $RunRoot "$role-size-components.log")
            $binary = Join-Path $entry.build 'cardputer_pocketjs.bin'
            Invoke-Logged $python @('tools/check_flash.py',$binary) (Join-Path $RunRoot "$role-flash-budget.log")
            $entry.binary = $binary
            $entry.binaryBytes = (Get-Item -LiteralPath $binary).Length
            $entry.binarySha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash
            $configPath = Join-Path $entry.build 'sdkconfig'
            $entry.sdkconfigSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash
            if (!(Select-String -LiteralPath $configPath -Pattern '^CONFIG_IDF_TARGET="esp32s3"$' -Quiet)) { throw 'Generated config target mismatch.' }
            if (Select-String -LiteralPath $configPath -Pattern '^CONFIG_POCKET_VM_(SELFTEST|OOMPROBE)=y$' -Quiet) { throw 'Unexpected diagnostic config.' }
            Get-GitText $entry.worktree @('status','--porcelain') | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $RunRoot "$role-worktree-status.log")
            # Verify the experimental flag reaches the real QuickJS compilation.
            if ($role -eq 'candidate' -and $Candidate -eq 'H') {
                $compile = Get-Content -Raw -LiteralPath (Join-Path $entry.build 'compile_commands.json') | ConvertFrom-Json
                $qjs = @($compile | Where-Object { $_.file -match '[/\\]quickjs\.c$' })
                $enabled = if ($EnableTypedPut) { 1 } else { 0 }
                if ($qjs.Count -ne 1 -or $qjs[0].command -notmatch "(?:^|\s)-DPOCKET_VM_TYPED_PUT_INT_FAST=$enabled(?:\s|$)") {
                    throw 'Typed-store flag was not verified in the QuickJS compile command.'
                }
            }
            if ($role -eq 'candidate' -and $EnablePrecompileExperiment) {
                $bytecode = @(Get-ChildItem -LiteralPath (Join-Path $entry.build 'experimental-bytecode') -Filter '*.bc' -File)
                if ($bytecode.Count -eq 0) { throw 'F requested bytecode but no experimental outputs were generated.' }
                $entry.experimentalBytecodeFiles = @($bytecode | ForEach-Object { $_.FullName })
            }
            $entry.status = 'built'
        } finally { Pop-Location }
        Write-Json $manifest $manifestPath
    }
    $manifest.status = 'built'
} catch {
    $manifest.status = 'failed'
    $manifest.failure = $_.Exception.Message
    throw
} finally { Write-Json $manifest $manifestPath }
Write-Host "Built baseline + $Candidate. Manifest: $manifestPath. No serial access or flashing performed."
