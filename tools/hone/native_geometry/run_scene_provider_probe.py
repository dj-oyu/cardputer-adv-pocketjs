"""Host-only real/synthetic rest-geometry probe; never firmware admission."""
import argparse,json,math,os,pathlib,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--blob',type=pathlib.Path,required=True)
p.add_argument('--geometry',type=pathlib.Path,required=True)
p.add_argument('--provider-map',type=pathlib.Path,required=True)
p.add_argument('--out',type=pathlib.Path,required=True)
p.add_argument('--sanitize',action='store_true')
p.add_argument('--scene-plan',type=pathlib.Path)
p.add_argument('--driver-nodes',type=pathlib.Path)
p.add_argument('--driver-programs',type=pathlib.Path)
p.add_argument('--morph-targets',type=pathlib.Path)
p.add_argument('--rig-targets',type=pathlib.Path)
p.add_argument('--wrap-targets',type=pathlib.Path)
p.add_argument('--positions-out',type=pathlib.Path,help='optional new private binary position samples for Blender comparison')
p.add_argument('--normals-out',type=pathlib.Path,help='optional new private packed corner normals for Blender comparison')
a=p.parse_args();here=pathlib.Path(__file__).resolve().parent;root=here.parents[2]
driver_args=(a.scene_plan,a.driver_nodes,a.driver_programs)
if any(driver_args) and not all(driver_args):p.error('all three driver probe inputs are required')
if a.morph_targets and not all(driver_args):p.error('morph targets require the driver probe inputs')
if a.rig_targets and not a.morph_targets:p.error('rig targets require the morph probe inputs')
if a.wrap_targets and not a.rig_targets:p.error('wrap targets require the rig probe inputs')
if a.positions_out and not a.wrap_targets:p.error('position samples require the wrap probe inputs')
if a.normals_out and not a.positions_out:p.error('normal samples require position samples')
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
shutil.copyfile(a.geometry,out/'geometry.h');shutil.copyfile(a.provider_map,out/'provider_nodes.h')
if all(driver_args):
 scene=json.loads(a.scene_plan.read_text(encoding='utf8'))
 if scene.get('schema')!='hone.scene-node-plan.v1' or scene.get('runtime_ready') is not False:raise ValueError('scene plan')
 nodes=scene['nodes'];values=[v for node in nodes for v in node['pose']]
 if len(nodes)>256 or len(values)!=len(nodes)*9 or any(type(v) not in (int,float) or not math.isfinite(v) for v in values):raise ValueError('scene poses')
 parents=[node['parent'] for node in nodes]
 if any(type(p) is not int or p < -1 or p >= i for i,p in enumerate(parents)):raise ValueError('scene parent order')
 meshes=[n['source_mesh'] for n in nodes if n['kind']=='gouraudMesh']
 if not meshes or any(type(m) is not int or m<0 or m>=1000000 for m in meshes):raise ValueError('scene mesh mapping')
 first=[0xffffffff]*(max(meshes)+1)
 for i,node in enumerate(nodes):
  if node['kind']=='gouraudMesh' and first[node['source_mesh']]==0xffffffff:first[node['source_mesh']]=i
 (out/'scene_pose.h').write_text('static const float hone_probe_base_pose[] = {'+','.join(float(v).hex()+'f' for v in values)+'};\n'
  +'static const size_t hone_probe_node_count = %d;\n'%len(nodes)
  +'#ifdef HONE_PROBE_WRAP\nstatic const int16_t hone_probe_parents[] = {'+','.join(map(str,parents))+'};\n'
  +'static const uint32_t hone_probe_mesh_first_node[] = {'+','.join(map(str,first))+'};\n'
  +'static const size_t hone_probe_mesh_count = %d;\n#endif\n'%len(first),encoding='utf8')
 shutil.copyfile(a.driver_nodes,out/'driver_nodes.h');shutil.copyfile(a.driver_programs,out/'driver_programs.h')
if a.morph_targets:shutil.copyfile(a.morph_targets,out/'morph_targets.h')
if a.rig_targets:shutil.copyfile(a.rig_targets,out/'rig_targets.h')
if a.wrap_targets:shutil.copyfile(a.wrap_targets,out/'wrap_targets.h')
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c',
         'main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c','tools/hone/source_package/native_wedge_lut_candidate.c',
         'tools/hone/source_package/rigid_dq_candidate.c']
if a.wrap_targets:sources.append('tools/hone/native_geometry/native_wrap_candidate.c')
binary=out/('probe.exe' if os.name=='nt' else 'probe')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O1' if a.sanitize else '-O2','-g','-Wall','-Wextra','-Werror',
     '-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER',
     '-I'+str(out),'-I'+str(here),'-I'+str(root/'tools/hone/source_package'),'-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),
     *[str(root/s) for s in sources],str(here/'scene_provider_probe.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
if all(driver_args):cmd.insert(1,'-DHONE_PROBE_NODE_DRIVER')
if a.morph_targets:cmd.insert(1,'-DHONE_PROBE_MORPH_DRIVER')
if a.rig_targets:cmd.insert(1,'-DHONE_PROBE_RIG_DRIVER')
if a.wrap_targets:cmd.insert(1,'-DHONE_PROBE_WRAP')
subprocess.run(cmd,check=True)
run=[str(binary),str(a.blob.resolve())]
if a.positions_out:run.append(str(a.positions_out.resolve()))
if a.normals_out:run.append(str(a.normals_out.resolve()))
subprocess.run(run,check=True)
