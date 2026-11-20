"""Synthetic Blender driver/coordinate/continuous-opacity contract, not a shader renderer test."""
import bpy,math
from mathutils import Vector,Matrix
bpy.ops.wm.read_factory_settings(use_empty=True)
control=bpy.data.objects.new('controls',None);bpy.context.collection.objects.link(control);control['both']=0.;control['side']=0.
m=bpy.data.materials.new('synthetic-mask');m.use_nodes=True;node=m.node_tree.nodes.new('ShaderNodeValue');node.name='driver';fc=node.outputs[0].driver_add('default_value');fc.driver.expression='max(a,b)'
for name,path in [('a','both'),('b','side')]:
 v=fc.driver.variables.new();v.name=name;v.type='SINGLE_PROP';v.targets[0].id=control;v.targets[0].data_path='["'+path+'"]'
mesh=bpy.data.meshes.new('mesh');mesh.from_pydata([(0,0,0),(1,0,0),(0,1,0)],[],[(0,1,2)]);obj=bpy.data.objects.new('render-mesh',mesh);bpy.context.collection.objects.link(obj);mesh.materials.append(m)
m.node_tree.links.new(node.outputs[0],m.node_tree.nodes['Principled BSDF'].inputs['Alpha'])
local=Vector((.25,.5,.75));results=[]
for frame,(both,side,angle) in enumerate([(0.,0.,0.),(.01,0.,.3),(0.,.05,-.2),(.03,.01,.5)]):
 control['both']=both;control['side']=side;control.update_tag();obj.matrix_world=Matrix.Translation((2.,-1.,.5)) @ Matrix.Rotation(angle,4,'Z');bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();dg.update()
 driver=node.outputs[0].default_value;assert abs(driver-max(both,side))<1e-7
 world=obj.evaluated_get(dg).matrix_world @ local;recovered=obj.evaluated_get(dg).matrix_world.inverted() @ world;assert (recovered-local).length<1e-6
 # Synthetic graph: opaque weight=max(inside,1-min(1,10*driver)).
 # Input order is transparent then opaque; use the full continuous factor.
 opacity=max(0.,1.-min(1.,10.*driver));results.append(opacity)
 assert 0<=opacity<=1 and frame==len(results)-1
assert abs(results[1]-.9)<1e-6 and abs(results[2]-.5)<1e-6 and abs(results[3]-.7)<1e-6
print('PASS four coherent driver/coordinate samples and fractional factors; GPU alpha equivalence untested')
