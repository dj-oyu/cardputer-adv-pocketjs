"""Native telemetry safety and shipping/generator isolation checks."""
from pathlib import Path
import subprocess
import tempfile
import importlib.util
import os

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('generator',ROOT/'tools/derby_telemetry_chunks.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    out=Path(directory)
    for flags in (['-O1','-g','-fsanitize=address,undefined'],['-O2','-fstrict-aliasing']):
        exe=out/'telemetry'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-DPOCKET_DERBY_TELEMETRY',
                        *flags,'-I',str(ROOT/'main/pocket'),str(ROOT/'tools/test_derby_telemetry.c'),
                        str(ROOT/'main/pocket/derby_telemetry.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    source=ROOT/'apps/derby'
    before={p.name:p.read_bytes() for p in source.glob('*.js')}
    module.generate(source,out/'chunks')
    text=(out/'chunks/derby_play.js').read_text()
    call=next(line for line in text.splitlines() if line.startswith('  S.view(0,'))
    assert 'F.seed,raceNo' in call and 'tier);' in call
    assert not any(s in call for s in ['[', '{', '=>', 'new '])
    assert before=={p.name:p.read_bytes() for p in source.glob('*.js')}
    assert text.replace(call+'\n','',1)==before['derby_play.js'].decode()
    assert text.count('S.view(0,')==1
    listing=(out/'chunks/chunks.txt').read_text()
    assert listing.count('scene ')==1 and listing.count('app local.derby')==1
    print('PASS diagnostic generation leaves all shipping chunks byte-identical')
    # Compile the actual numeric native boundary against the real QuickJS.
    native=(ROOT/'main/pocket/pocket_derby.c').read_text()
    number=native[native.index('static bool number('):native.index('/* A plan handle')]
    pos=native.index('static JSValue js_view(')
    start=native.index('#ifdef POCKET_DERBY_TELEMETRY',pos)
    end=native.index('#endif',start)+len('#endif')
    body=native[start:end]
    (out/'derby_context.inc').write_text(number+'\nstatic JSValue context_call(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;\n'+body+'\nreturn JS_UNDEFINED;}\n')
    qjs=ROOT/'components/quickjs-ng/quickjs-ng'
    cache=ROOT/'.cache/derby_telemetry_vm';cache.mkdir(parents=True,exist_ok=True)
    objects=[]
    for name in ['dtoa','libregexp','libunicode','quickjs','quickjs-vm']:
        obj=cache/(name+'.o');src=qjs/(name+'.c')
        if not obj.exists() or src.stat().st_mtime>obj.stat().st_mtime:
            subprocess.run(['gcc','-std=gnu11','-O1','-g','-w','-fsanitize=address,undefined',
                '-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-I',str(qjs),'-I',str(ROOT/'components/pocketjs_guest/include'),
                '-c',str(src),'-o',str(obj)],check=True)
        objects.append(str(obj))
    exe=out/'vm'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DPOCKET_DERBY_TELEMETRY','-I',str(qjs),'-I',str(out),'-I',str(ROOT/'main/pocket'),
        str(ROOT/'tools/test_derby_telemetry_vm.c'),str(ROOT/'main/pocket/derby_telemetry.c'),
        *objects,'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1'))
