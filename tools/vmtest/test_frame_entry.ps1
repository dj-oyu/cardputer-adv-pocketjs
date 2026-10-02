param([string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $repo '.cache/frame-entry-host'
New-Item -ItemType Directory -Force $out | Out-Null
'/* host diagnostic; no target config */' | Set-Content (Join-Path $out 'sdkconfig.h') -Encoding ascii
$env:PATH = (Split-Path $Compiler) + ';' + $env:PATH
& python (Join-Path $PSScriptRoot 'frame_entry_source.py') (Join-Path $out 'frame_entry_source.inc')
if($LASTEXITCODE -ne 0) {throw 'Source entry extraction failed'}
$q = Join-Path $repo 'components/quickjs-ng/quickjs-ng'
$g = Join-Path $repo 'components/pocketjs_guest'
$flags = @('-std=gnu11','-O2','-g','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-I',$q,'-I',(Join-Path $g 'include'),'-I',$out,'-I',(Join-Path $PSScriptRoot 'frame_entry_shim'),'-I',(Join-Path $repo 'tools/hostshim'),'-I',(Join-Path $repo 'main/ui'),'-I',(Join-Path $repo 'main/text'),'-I',(Join-Path $repo 'main/pocket'))
$sources = @('dtoa','libregexp','libunicode','quickjs','quickjs-libc','quickjs-vm') | ForEach-Object {Join-Path $q ($_ + '.c')}
$sources += @((Join-Path $g 'src/guest.c'),(Join-Path $g 'src/vm_sched.c'),(Join-Path $g 'src/vm_clock.c'),(Join-Path $g 'src/block_cache.c'),(Join-Path $repo 'main/ui/jsconsole.c'),(Join-Path $PSScriptRoot 'test_frame_entry.c'))
$objects = @()
foreach($source in $sources) {
    $obj = Join-Path $out (([IO.Path]::GetFileNameWithoutExtension($source)) + '.o')
    $warnings = @('-Wall','-Wextra','-Wno-unused-parameter')
    if($source -notlike "$q*") { $warnings = @('-Wall','-Wextra','-Werror') }
    & $Compiler @flags @warnings '-c' $source '-o' $obj
    if($LASTEXITCODE -ne 0) {throw "Compile failed: $source"}
    $objects += $obj
}
$exe = Join-Path $out 'test-frame-entry.exe'
& $Compiler @objects '-lm' '-lpthread' '-o' $exe
if($LASTEXITCODE -ne 0) {throw 'Link failed'}
& $exe
if($LASTEXITCODE -ne 0) {throw 'Frame entry regression failed'}
