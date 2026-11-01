"""Integer-center depth oracle for offline fox variants; host analysis only."""
import hashlib
import json
from pathlib import Path
from test_fox_depth import ROOT, f32, project, compare


def main():
    folder=ROOT/'.cache/fox-compiled-assets'
    paths=[folder/(name+'.json') for name in ('fox-patch-compiled','fox-patch-eye','fox-patch-mask','fox-patch-shape','fox-patch-combined')]
    inputs=paths+[Path(__file__),ROOT/'tools/character/test_fox_depth.py']
    before={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    rows=[]
    for path in paths:
        manifest=json.loads(path.read_text())
        meshes=[n['mesh'] for n in manifest['nodes'] if n['kind']=='gouraudMesh']
        assert len(meshes)==1
        mesh=meshes[0]
        xyz=[tuple(map(f32,mesh['vertices'][i:i+3])) for i in range(0,len(mesh['vertices']),3)]
        tris=[mesh['indices'][i:i+3] for i in range(0,len(mesh['indices']),3)]
        assert len(xyz)==30 and len(tris)==46
        for pitch in (-10,0,10):
            for yaw in (-35,0,35):
                result,*_=compare(project(xyz,yaw,pitch),tris)
                assert result['covered_pixels']>0 and result['mismatch_pixels']==0,(path.name,yaw,pitch,result)
                rows.append(dict(asset=path.stem,yaw=yaw,pitch=pitch,**result))
        negative,*_=compare(project(xyz,-35,0),tris,reverse=True)
        # A view with overlap must detect deliberately reversed painter ordering.
        assert negative['overlap_pixels']==0 or negative['mismatch_pixels']>0
        print('COMPILED_DEPTH',path.stem,'views=9 mismatch=0 reverse_mismatch='+str(negative['mismatch_pixels']))
    unchanged=before=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    assert unchanged
    proof=dict(kind='HOST GEOMETRY ORACLE / NOT DEVICE OR GENERAL Z BUFFER',passed=True,
        source_sha256=before,sources_unchanged=True,views=rows,
        coverage='integer centers, inclusive ceil(min)..floor(max), canonical edges and top-left',
        scope='Five authored sheets, nine sampled view directions each; excludes separate eye/nose/skull occlusion and live app pitch support.')
    out=ROOT/'.cache/fox-compiled-depth';out.mkdir(parents=True,exist_ok=True)
    (out/'result.json').write_text(json.dumps(proof,indent=2)+'\n')
    print('COMPILED_FOX_DEPTH_PASS views='+str(len(rows)))


if __name__=='__main__':
    main()
