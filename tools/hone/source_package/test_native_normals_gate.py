"""Reproducible host normal gate, no firmware or runtime admission.

The exact-angle baseline and independent measured-wedge LUT are built anew.
Synthetic Blender tests check near-collapse and held-out wedge behavior. The
LUT then runs through the actual public Model/snapshot/raster integration test.
An optional private export is consumed read-only and only aggregate errors are
printed. No geometry, names or per-corner data are copied to generated files.
LUT storage is 65 binary32 samples (260 bytes); workspace size is unchanged.
Accuracy here is Blender 4.3.2 specific, not evidence of device speed or FPS.
"""
import argparse, json, os, pathlib, subprocess, sys
from compare_rest_normals import compare


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--blender',type=pathlib.Path,required=True)
    p.add_argument('--out',type=pathlib.Path,required=True)
    p.add_argument('--private-source',type=pathlib.Path)
    p.add_argument('--opt',choices=['0','2'],default='2')
    a=p.parse_args();here=pathlib.Path(__file__).resolve().parent
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    libraries={}
    for mode,source in [('exact','native_polygon_normals.c'),('wedge','native_wedge_lut_candidate.c')]:
        library=out/(mode+('.dll' if os.name=='nt' else '.so'))
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-Wall','-Wextra','-Werror','-fPIC','-shared','-ffp-contract=off','-fno-fast-math',str(here/source),'-lm','-o',str(library)],check=True)
        libraries[mode]=library
        subprocess.run([str(a.blender.resolve()),'-b','--factory-startup','--python-exit-code','1','--python',str(here/'test_native_blender_oracle.py'),'--',str(library)],check=True)
    subprocess.run([str(a.blender.resolve()),'-b','--factory-startup','--python-exit-code','1','--python',str(here/'test_wedge_lut_oracle.py'),'--',str(libraries['wedge'])],check=True)
    unit=out/('wedge-unit.exe' if os.name=='nt' else 'wedge-unit')
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-ffp-contract=off','-fno-fast-math',str(here/'test_wedge_lut.c'),'-lm','-o',str(unit)],check=True)
    subprocess.run([str(unit)],check=True)
    subprocess.run([sys.executable,str(here.parent/'native_geometry'/'test_native_deformed_provider_integration.py'),'--build-dir',str(out/'provider'),'--opt',a.opt,'--normals','wedge'],check=True)
    if a.private_source:
        package=json.loads(a.private_source.read_text(encoding='utf8'))
        results={mode:compare(package,library,1e-4) for mode,library in libraries.items()}
        print(json.dumps(results,sort_keys=True))
        result=results['wedge']
        if not result['corners'] or result['rejected_meshes'] or result['over_tolerance']:
            raise SystemExit('candidate failed optional private aggregate gate')
    print('PASS normal gate: synthetic Blender + Model/snapshot/raster; runtime_ready=false')


if __name__=='__main__':main()
