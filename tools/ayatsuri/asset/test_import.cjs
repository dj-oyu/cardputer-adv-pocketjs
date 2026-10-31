// Synthetic GLB fixtures only; this does not establish real Blender compatibility.
'use strict';
const assert=require('node:assert/strict');
const {importGlb,ImportError}=require('./import_glb.cjs');
function makeGlb(json,bin){
  const text=Buffer.from(JSON.stringify(json),'utf8'),j=Buffer.alloc((text.length+3)&~3,32);text.copy(j);
  const b=Buffer.alloc((bin.length+3)&~3);bin.copy(b);const out=Buffer.alloc(12+8+j.length+8+b.length);
  out.write('glTF');out.writeUInt32LE(2,4);out.writeUInt32LE(out.length,8);out.writeUInt32LE(j.length,12);out.writeUInt32LE(0x4e4f534a,16);j.copy(out,20);
  const at=20+j.length;out.writeUInt32LE(b.length,at);out.writeUInt32LE(0x004e4942,at+4);b.copy(out,at+8);return out;
}
function triangleFixture(options={}){
  const positions=options.positions||[[0,0,0],[1,0,0],[0,1,0]],normals=options.normals||positions.map(()=>[0,0,1]),indices=options.indices||[0,1,2],interleaved=!!options.interleaved,indexType=options.indexType||5123;
  const size=({5121:1,5123:2,5125:4})[indexType],vertexBytes=positions.length*24,bin=Buffer.alloc(vertexBytes+indices.length*size);
  positions.forEach((p,i)=>p.forEach((v,j)=>bin.writeFloatLE(v,interleaved?i*24+j*4:i*12+j*4)));
  normals.forEach((p,i)=>p.forEach((v,j)=>bin.writeFloatLE(v,interleaved?i*24+12+j*4:positions.length*12+i*12+j*4)));
  indices.forEach((v,i)=>size===1?bin.writeUInt8(v,vertexBytes+i):size===2?bin.writeUInt16LE(v,vertexBytes+i*2):bin.writeUInt32LE(v,vertexBytes+i*4));
  const views=interleaved?[{buffer:0,byteOffset:0,byteLength:vertexBytes,byteStride:24,target:34962}]:[
    {buffer:0,byteOffset:0,byteLength:positions.length*12,target:34962},{buffer:0,byteOffset:positions.length*12,byteLength:normals.length*12,target:34962}];
  const indexView=views.length;views.push({buffer:0,byteOffset:vertexBytes,byteLength:indices.length*size,target:34963});
  const json={asset:{version:'2.0',generator:'synthetic-test'},scene:0,scenes:[{nodes:[0]}],nodes:[{name:'狐',mesh:0}],meshes:[{name:'head',primitives:[{attributes:{POSITION:0,NORMAL:1},indices:2,material:0}]}],
    buffers:[{byteLength:bin.length}],bufferViews:views,accessors:[
      {bufferView:0,byteOffset:0,componentType:5126,count:positions.length,type:'VEC3',min:[0,1,2].map(k=>Math.min(...positions.map(v=>v[k]))),max:[0,1,2].map(k=>Math.max(...positions.map(v=>v[k])))},
      {bufferView:interleaved?0:1,byteOffset:interleaved?12:0,componentType:5126,count:normals.length,type:'VEC3'},
      {bufferView:indexView,componentType:indexType,count:indices.length,type:'SCALAR'}],materials:[{name:'fur',pbrMetallicRoughness:{baseColorFactor:[.7,.3,.1,1],metallicFactor:0,roughnessFactor:.8}}]};
  return {json,bin,glb:makeGlb(json,bin)};
}
function runTests(){let tests=0;const test=(name,fn)=>{try{fn();tests++;}catch(e){e.message=`${name}: ${e.message}`;throw e;}};
  const approx=(a,b,t=1e-12)=>{assert.equal(a.length,b.length);a.forEach((x,i)=>assert.ok(Math.abs(x-b[i])<=t,`${a} != ${b}`));};
  const reject=(name,mutate,code='INVALID_GLB',options={})=>test(name,()=>{const f=triangleFixture();mutate(f.json,f.bin);assert.throws(()=>importGlb(makeGlb(f.json,f.bin),options),e=>e instanceof ImportError&&e.code===code&&typeof e.path==='string');});
  test('basis and stable correspondence',()=>{const f=triangleFixture(),before=Buffer.from(f.glb),ir=importGlb(f.glb);assert.ok(f.glb.equals(before));
    assert.equal(ir.schema,'hone-scene-ir-v1');assert.equal(ir.nodes[0].name,'狐');assert.equal(ir.surfaces[0].stableId,'node/0/mesh/0/primitive/0');
    assert.deepEqual(ir.surfaces[0].positions,[[0,0,0],[1,0,0],[0,-1,0]]);assert.deepEqual(ir.surfaces[0].triangles,[[0,2,1]]);
    assert.deepEqual(ir.surfaces[0].sourceVertexIds,[0,1,2]);assert.deepEqual(ir.materials[0].baseColor,[.7,.3,.1,1]);
    assert.deepEqual(ir.materials[0].sourceProperties,{metallicFactor:0,roughnessFactor:.8});f.glb.fill(0);assert.equal(ir.surfaces[0].positions[1][0],1);});
  test('input subarray honors its bounds',()=>{const f=triangleFixture(),b=Buffer.concat([Buffer.alloc(13),f.glb,Buffer.alloc(7)]);assert.deepEqual(importGlb(b.subarray(13,-7)),importGlb(f.glb));});
  test('canonical signed zero and JSON roundtrip',()=>{
    const f=triangleFixture({positions:[[-0,-0,-0],[1,-0,-0],[-0,1,-0]],normals:Array(3).fill([-0,-0,1])});
    f.json.nodes[0].translation=[.5,0,0];f.json.nodes[0].extras={nested:[{signed:.5}]};
    f.json.materials[0].pbrMetallicRoughness={baseColorFactor:[.5,.3,.1,1],metallicFactor:.5,roughnessFactor:.5};
    f.json.materials[0].extras={signed:.5};
    const b=makeGlb(f.json,f.bin),jsonLength=b.readUInt32LE(12);
    // Preserve literal -0 in JSON; JSON.stringify itself would erase it.
    b.write(b.toString('utf8',20,20+jsonLength).replace(/0\.5/g,'-0 '),20,'utf8');
    assert.ok(Object.is(JSON.parse(b.toString('utf8',20,20+jsonLength)).nodes[0].extras.nested[0].signed,-0));
    const before=Buffer.from(b),ir=importGlb(b);
    assert.deepEqual(ir,JSON.parse(JSON.stringify(ir)));
    const pending=[ir];while(pending.length){const v=pending.pop();assert.ok(!Object.is(v,-0));if(v!==null&&typeof v==='object')pending.push(...Object.values(v));}
    assert.equal(ir.materials[0].baseColor[0],0);assert.equal(ir.materials[0].sourceProperties.metallicFactor,0);
    assert.equal(ir.nodes[0].extras.nested[0].signed,0);assert.ok(b.equals(before));
    assert.equal(ir.source.sha256,require('node:crypto').createHash('sha256').update(before).digest('hex'));
  });
  for(const type of [5121,5123,5125])test(`index type ${type}`,()=>assert.deepEqual(importGlb(triangleFixture({indexType:type}).glb).surfaces[0].triangles,[[0,2,1]]));
  test('interleaved attributes',()=>assert.deepEqual(importGlb(triangleFixture({interleaved:true}).glb).surfaces[0].positions,[[0,0,0],[1,0,0],[0,-1,0]]));
  test('nonindexed triangles',()=>{const f=triangleFixture();delete f.json.meshes[0].primitives[0].indices;assert.deepEqual(importGlb(makeGlb(f.json,f.bin)).surfaces[0].triangles,[[0,2,1]]);});
  test('negative scale cancels basis reflection once',()=>{const f=triangleFixture();f.json.nodes[0].scale=[-1,1,1];const s=importGlb(makeGlb(f.json,f.bin)).surfaces[0];assert.deepEqual(s.triangles,[[0,1,2]]);approx(s.normals[0],[0,0,1]);});
  test('parent hierarchy and quaternion',()=>{const f=triangleFixture();f.json.nodes=[{translation:[1,2,3],children:[1]},{mesh:0,rotation:[0,0,Math.SQRT1_2,Math.SQRT1_2]}];const ir=importGlb(makeGlb(f.json,f.bin));assert.equal(ir.nodes[1].parentId,'node/0');approx(ir.surfaces[0].positions[1],[1,-3,3]);});
  test('inverse transpose under nonuniform scale',()=>{const f=triangleFixture({normals:Array(3).fill([1/Math.sqrt(3),1/Math.sqrt(3),1/Math.sqrt(3)])});f.json.nodes[0].scale=[2,3,4];const n=[.5,-1/3,.25],len=Math.hypot(...n);approx(importGlb(makeGlb(f.json,f.bin)).surfaces[0].normals[0],n.map(v=>v/len));});
  test('static matrix/shear',()=>{const f=triangleFixture({normals:Array(3).fill([1,0,0])});f.json.nodes[0].matrix=[1,0,0,0,1,1,0,0,0,0,1,0,4,5,6,1];const s=importGlb(makeGlb(f.json,f.bin)).surfaces[0];approx(s.positions[2],[5,-6,6]);approx(s.normals[0],[Math.SQRT1_2,Math.SQRT1_2,0]);});
  test('mesh instancing preserves source IDs',()=>{const f=triangleFixture();f.json.nodes.push({mesh:0,translation:[10,0,0]});f.json.scenes[0].nodes.push(1);const ir=importGlb(makeGlb(f.json,f.bin));assert.equal(ir.surfaces.length,2);assert.equal(ir.counts.vertices,6);assert.equal(ir.surfaces[1].sourceMesh,0);assert.equal(ir.surfaces[1].positions[0][0],10);});
  test('deterministic and default material retained',()=>{const f=triangleFixture();delete f.json.meshes[0].primitives[0].material;const b=makeGlb(f.json,f.bin);assert.deepEqual(importGlb(b),importGlb(b));assert.equal(importGlb(b).surfaces[0].materialId,null);assert.equal(importGlb(b).defaultMaterial.baseColor[3],1);});
  reject('out of bounds view',g=>g.bufferViews[0].byteLength=g.buffers[0].byteLength+1);
  reject('out of bounds accessor',g=>g.accessors[0].count=100);
  reject('unsafe offset',g=>g.accessors[0].byteOffset=Number.MAX_SAFE_INTEGER);
  reject('null offset',g=>g.accessors[0].byteOffset=null);
  reject('unaligned accessor',g=>g.accessors[0].byteOffset=1);
  reject('invalid stride',g=>g.bufferViews[0].byteStride=13);
  reject('stride too small',g=>g.bufferViews[0].byteStride=4);
  reject('index stride',g=>g.bufferViews[2].byteStride=4);
  reject('position component',g=>g.accessors[0].componentType=5125);
  reject('index range',(_g,b)=>b.writeUInt16LE(3,76));
  reject('duplicate indices',(_g,b)=>b.writeUInt16LE(1,76));
  reject('zero area',(_g,b)=>{b.writeFloatLE(0,24);b.writeFloatLE(0,28);});
  reject('NaN vertex',(_g,b)=>b.writeFloatLE(NaN,0));
  reject('infinite normal',(_g,b)=>b.writeFloatLE(Infinity,36));
  reject('zero normal',(_g,b)=>b.writeFloatLE(0,44));
  reject('min bounds violated',g=>g.accessors[0].min=[.5,0,0]);
  reject('normal count mismatch',g=>g.accessors[1].count=2);
  reject('missing normals',g=>delete g.meshes[0].primitives[0].attributes.NORMAL);
  reject('mode',g=>g.meshes[0].primitives[0].mode=5,'UNSUPPORTED');
  reject('UV',g=>g.meshes[0].primitives[0].attributes.TEXCOORD_0=1,'UNSUPPORTED');
  reject('vertex colors',g=>g.meshes[0].primitives[0].attributes.COLOR_0=1,'UNSUPPORTED');
  reject('morph',g=>g.meshes[0].primitives[0].targets=[],'UNSUPPORTED');
  reject('skin',g=>g.nodes[0].skin=0,'UNSUPPORTED');
  reject('animation',g=>g.animations=[{}],'UNSUPPORTED');
  reject('camera',g=>g.nodes[0].camera=0,'UNSUPPORTED');
  reject('URI',g=>g.buffers[0].uri='file:///secret','UNSUPPORTED');
  reject('sparse',g=>g.accessors[0].sparse={},'UNSUPPORTED');
  reject('normalized',g=>g.accessors[0].normalized=true,'UNSUPPORTED');
  reject('extension root',g=>g.extensionsUsed=['KHR_unknown'],'UNSUPPORTED');
  reject('extension primitive',g=>g.meshes[0].primitives[0].extensions={KHR_draco_mesh_compression:{}},'UNSUPPORTED');
  reject('alpha',g=>g.materials[0].alphaMode='BLEND','UNSUPPORTED');
  reject('factor alpha',g=>g.materials[0].pbrMetallicRoughness.baseColorFactor[3]=.5,'UNSUPPORTED');
  reject('double sided',g=>g.materials[0].doubleSided=true,'UNSUPPORTED');
  reject('texture',g=>g.materials[0].pbrMetallicRoughness.baseColorTexture={index:0},'UNSUPPORTED');
  reject('emission',g=>g.materials[0].emissiveFactor=[1,0,0],'UNSUPPORTED');
  reject('null PBR',g=>g.materials[0].pbrMetallicRoughness=null);
  reject('control in name',g=>g.nodes[0].name='bad\u0000name');
  reject('unpaired surrogate name',g=>g.nodes[0].name='\ud800');
  reject('matrix plus TRS',g=>{g.nodes[0].matrix=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];g.nodes[0].scale=[1,1,1];});
  reject('non affine',g=>g.nodes[0].matrix=[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1]);
  reject('singular scale',g=>g.nodes[0].scale=[0,1,1]);
  reject('nonunit quaternion',g=>g.nodes[0].rotation=[0,0,0,2]);
  reject('duplicate root',g=>g.scenes[0].nodes=[0,0]);
  reject('cycle',g=>g.nodes[0].children=[0]);
  reject('unreachable',g=>g.nodes.push({}),'UNSUPPORTED');
  reject('multiple parents',g=>{g.nodes=[{children:[2]},{children:[2]},{mesh:0}];g.scenes[0].nodes=[0,1];});
  reject('depth cap',g=>{g.nodes=[{children:[1]},{mesh:0}];},'LIMIT',{maxDepth:1});
  reject('expanded vertex cap',g=>{g.nodes.push({mesh:0});g.scenes[0].nodes.push(1);},'LIMIT',{maxVertices:5});
  reject('triangle cap',g=>{g.nodes.push({mesh:0});g.scenes[0].nodes.push(1);},'LIMIT',{maxTriangles:1});
  reject('decoded bytes cap',()=>{},'LIMIT',{maxDecodedBytes:8});
  test('input byte cap',()=>assert.throws(()=>importGlb(triangleFixture().glb,{maxBytes:20}),e=>e.code==='LIMIT'));
  test('JSON cap',()=>assert.throws(()=>importGlb(triangleFixture().glb,{maxJsonBytes:8}),e=>e.code==='LIMIT'));
  test('GLB header mutations',()=>{for(const offset of [0,4,8,12,16]){const b=triangleFixture().glb;b.writeUInt32LE(0xffffffff,offset);assert.throws(()=>importGlb(b),ImportError);}});
  test('truncation all bytes',()=>{const b=triangleFixture().glb;for(let i=0;i<b.length;i++)assert.throws(()=>importGlb(b.subarray(0,i)),ImportError);});
  test('wrong BIN chunk',()=>{const b=triangleFixture().glb,at=20+b.readUInt32LE(12);b.writeUInt32LE(0x4e4f534a,at+4);assert.throws(()=>importGlb(b),ImportError);});
  test('invalid UTF8',()=>{const b=triangleFixture().glb;b[20]=255;assert.throws(()=>importGlb(b),ImportError);});
  test('shared buffer rejected',()=>{const b=triangleFixture().glb,s=new Uint8Array(new SharedArrayBuffer(b.length));s.set(b);assert.throws(()=>importGlb(s),e=>e.code==='UNSUPPORTED');});
  console.log(`IMPORT_GLB_SYNTHETIC_PASS tests=${tests} real_blender_export=NOT_TESTED`);return tests;
}
module.exports={makeGlb,triangleFixture,runTests};if(require.main===module)runTests();
