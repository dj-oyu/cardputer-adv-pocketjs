// AYNM v1 offline compiler. Runtime consumes immutable native data, not JS defs.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {exportCloud,REF}=require('./export_cloud.cjs');
function int(v,a,b){if(!Number.isInteger(v)||v<a||v>b)throw TypeError('integer range');return v;}
function f32(v){if(typeof v!=='number'||!Number.isFinite(Math.fround(v)))throw TypeError('finite F32');return v;}
function unit(v){if(typeof v!=='number'||!Number.isFinite(v)||v<0||v>1)throw TypeError('unit coefficient');return v;}
function floats(a,n){if(!a||a.length!==n)throw TypeError('vector length');for(let i=0;i<n;i++)f32(a[i]);return a;}
function meshRows(mesh,vertexStart,triangleStart,holeStart){
  if(!mesh||typeof mesh!=='object')throw TypeError('mesh descriptor');
  const nv=int(mesh.vertices?.length/3,3,32),nt=int(mesh.indices?.length/3,1,64),
    xyz=Array.from(floats(mesh.vertices,nv*3),Math.fround),mask=Array.from(floats(mesh.mask,nv),Math.fround),
    indices=Array.from(mesh.indices,v=>int(v,0,nv-1));
  if([...xyz,...mask].some(v=>Math.abs(v)>4096))throw TypeError('mesh coordinate/mask range');
  const normals=new Float32Array(nv*3);
  for(let t=0;t<nt;t++){
    const ids=indices.slice(t*3,t*3+3),a=ids[0]*3,b=ids[1]*3,c=ids[2]*3,
      ux=xyz[b]-xyz[a],uy=xyz[b+1]-xyz[a+1],uz=xyz[b+2]-xyz[a+2],
      vx=xyz[c]-xyz[a],vy=xyz[c+1]-xyz[a+1],vz=xyz[c+2]-xyz[a+2],
      n=[uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx];
    if(n.every(v=>v===0))throw TypeError('degenerate mesh triangle');
    // Match each authoring Float32 accumulation, then the final Float32 store.
    for(const id of ids)for(let j=0;j<3;j++)normals[id*3+j]+=n[j];
  }
  if(mesh.normals!==undefined)normals.set(floats(mesh.normals,nv*3));
  else for(let v=0;v<nv;v++){
    const at=v*3,length=Math.hypot(Math.hypot(normals[at],normals[at+1]),normals[at+2]);
    if(!(length>0))throw TypeError('undefined mesh normal');
    for(let j=0;j<3;j++)normals[at+j]/=length;
  }
  for(let v=0;v<nv;v++){
    const length=normals[v*3]**2+normals[v*3+1]**2+normals[v*3+2]**2;
    if(!Number.isFinite(length)||Math.abs(length-1)>1e-4)throw TypeError('unit mesh normal');
  }
  const holes=mesh.holes===undefined?[]:mesh.holes;
  if(!Array.isArray(holes)||holes.length>2)throw TypeError('mesh holes');
  const counts=holes.map(h=>int(h.length/3,3,8)),holeXYZ=[];
  for(let h=0;h<holes.length;h++)for(const value of floats(holes[h],counts[h]*3)){
    const v=Math.fround(value);if(Math.abs(v)>4096)throw TypeError('mesh hole range');holeXYZ.push(v);
  }
  const order=mesh.order===undefined?'authored':mesh.order;
  if(order!=='authored'&&order!=='centroid-depth')throw TypeError('mesh order');
  const descriptor=Buffer.alloc(32),vertices=Buffer.alloc(nv*28),triangles=Buffer.alloc(nt*6),holeBytes=Buffer.alloc(holeXYZ.length*4);
  [vertexStart,nv,triangleStart,nt,holeStart].forEach((v,j)=>descriptor.writeUInt16LE(int(v,0,65535),j*2));
  descriptor[10]=holes.length;descriptor[11]=order==='centroid-depth'?1:0;
  descriptor[12]=counts[0]||0;descriptor[13]=counts[1]||0;
  descriptor.writeUInt16LE(int(mesh.albedoA,0,65535),14);descriptor.writeUInt16LE(int(mesh.albedoB,0,65535),16);
  descriptor[18]=int(mesh.thickness,0,255);
  for(let v=0;v<nv;v++){
    for(let j=0;j<3;j++){vertices.writeFloatLE(xyz[v*3+j],v*28+j*4);vertices.writeFloatLE(normals[v*3+j],v*28+12+j*4);}
    vertices.writeFloatLE(mask[v],v*28+24);
  }
  indices.forEach((v,j)=>triangles.writeUInt16LE(v,j*2));holeXYZ.forEach((v,j)=>holeBytes.writeFloatLE(v,j*4));
  return {descriptor,vertices,triangles,holeBytes,nv,nt,nh:holeXYZ.length/3};
}
function compile(m){
  if(!m||m.schema!=='ayatsuri-native-model-design-v1'||!Array.isArray(m.nodes))throw TypeError('manifest');
  const count=int(m.nodes.length,1,256),nodes=Buffer.alloc(count*16),poses=Buffer.alloc(count*36),radii=[],lobes=[],materials=[],bindings=Buffer.alloc(count*8),names=[],regions=[],meshes=[],meshVertices=[],meshTriangles=[],meshHoles=[];
  let nameBytes=0,ellipseCount=0,lobeCount=0,meshVertexCount=0,meshTriangleCount=0,meshHoleCount=0;
  for(let i=0;i<count;i++){
    const n=m.nodes[i],kind={group:0,ellipsoid:1,blendEllipsoids:2,gouraudMesh:3}[n.kind];
    int(kind,0,3);if(n.index!==i||typeof n.enabled!=='boolean'||typeof n.name!=='string'||!(/^[\x20-\x7e]{1,127}$/).test(n.name)||names.includes(n.name))throw TypeError('identity');
    if(n.pattern!==undefined&&n.pattern!==null){
      const p=n.pattern;if((kind!==1&&kind!==2)||p.kind!=='localPlane')throw TypeError('analytic localPlane pattern');
      const normal=Array.from(floats(p.normal,3),Math.fround),length=normal.reduce((s,v)=>s+v*v,0);
      if(Math.abs(length-1)>1e-4)throw TypeError('unit localPlane normal');
      f32(p.offset);f32(p.feather);
      const limit=kind===1?2:4096;
      if(p.offset< -limit||p.offset>limit||(p.feather!==0&&(p.feather<1e-4||p.feather>limit)))throw TypeError('localPlane range');
      const region=Buffer.alloc(32);region.writeUInt16LE(i,0);region[2]=kind===1?4:5;
      normal.forEach((v,k)=>region.writeFloatLE(v,4+k*4));region.writeFloatLE(p.offset,16);region.writeFloatLE(p.feather,20);
      region.writeUInt32LE(int(p.color,0,0xffffff),24);regions.push(region);
    }
    int(n.parent,-1,i-1);floats(n.pose,9);for(let k=6;k<9;k++)if(Math.fround(n.pose[k])===0)throw TypeError('zero scale');
    n.pose.forEach((v,k)=>poses.writeFloatLE(v,i*36+k*4));
    let shape=65535,material=65535,lc=0,blend=0;
    if(kind){
      if(kind===3&&(n.thickness!==1||n.opacity!==1||n.edgeSoftness!==0))throw TypeError('mesh common thickness/opacity/edgeSoftness must be 1/1/0');
      const mat=Buffer.alloc(72);floats(n.material,11).forEach((v,k)=>{
        if(k===3){if(v<1||v>128)throw TypeError('exponent');}else unit(v);mat.writeFloatLE(v,k*4);
      });
      int(n.color,0,0xffffff);mat.writeUInt32LE(n.color,44);mat.writeDoubleLE(unit(n.thickness),48);mat.writeDoubleLE(unit(n.opacity),56);mat.writeDoubleLE(unit(n.edgeSoftness),64);
      material=materials.findIndex(b=>b.equals(mat));if(material<0){material=materials.length;materials.push(mat);}
      if(kind===1){floats(n.radii,3);n.radii.forEach(v=>{if(!(Math.fround(v)>0))throw TypeError('radii');});shape=ellipseCount++;const b=Buffer.alloc(12);n.radii.forEach((v,k)=>b.writeFloatLE(v,k*4));radii.push(b);}
      else if(kind===3){
        const mesh=meshRows(n.mesh,meshVertexCount,meshTriangleCount,meshHoleCount);shape=meshes.length;meshes.push(mesh.descriptor);
        meshVertices.push(mesh.vertices);meshTriangles.push(mesh.triangles);meshHoles.push(mesh.holeBytes);
        meshVertexCount+=mesh.nv;meshTriangleCount+=mesh.nt;meshHoleCount+=mesh.nh;
      }else{lc=int(n.lobes.length/6,1,8);floats(n.lobes,lc*6);shape=lobeCount;lobeCount+=lc;blend=f32(n.blend);if(blend<0||blend>128)throw TypeError('blend');
        const b=Buffer.alloc(lc*24);n.lobes.forEach((v,k)=>{if(k%6>=3&&!(Math.fround(v)>0))throw TypeError('lobe radii');b.writeFloatLE(v,k*4);});lobes.push(b);}
    }
    const o=i*16;nodes.writeInt16LE(n.parent,o);nodes[o+2]=kind;nodes[o+3]=n.enabled?1:0;nodes.writeUInt16LE(shape,o+4);nodes.writeUInt16LE(lc,o+6);nodes.writeUInt16LE(material,o+8);nodes.writeUInt16LE(i,o+10);nodes.writeFloatLE(blend,o+12);
    bindings.writeUInt16LE(i,i*8);bindings.writeUInt16LE(n.name.length,i*8+2);bindings.writeUInt32LE(nameBytes,i*8+4);nameBytes+=n.name.length;names.push(n.name);
  }
  const sections=[{data:nodes,count,stride:16},{data:poses,count,stride:36},{data:Buffer.concat(radii),count:ellipseCount,stride:12},{data:Buffer.concat(lobes),count:lobeCount,stride:24},{data:Buffer.concat(materials),count:materials.length,stride:72},{data:bindings,count,stride:8},{data:Buffer.from(names.join(''),'ascii'),count:nameBytes,stride:1}];
  if(regions.length)sections.push({data:Buffer.concat(regions),count:regions.length,stride:32});
  if(meshes.length)sections.push(
    {type:9,data:Buffer.concat(meshes),count:meshes.length,stride:32},
    {type:10,data:Buffer.concat(meshVertices),count:meshVertexCount,stride:28},
    {type:11,data:Buffer.concat(meshTriangles),count:meshTriangleCount,stride:6},
    {type:12,data:Buffer.concat(meshHoles),count:meshHoleCount,stride:12});
  let total=32+sections.length*20;for(const s of sections){total=(total+7)&~7;s.offset=total;total+=s.data.length;}int(total,32,512*1024);
  const out=Buffer.alloc(total);out.write('AYNM');out.writeUInt16LE(1,4);out.writeUInt16LE(32,6);out.writeUInt32LE(total,8);out.writeUInt32LE(sections.length,12);out.writeUInt32LE(32,16);out.writeUInt32LE(20,20);
  sections.forEach((s,i)=>{const o=32+i*20;out.writeUInt16LE(s.type||i+1,o);out.writeUInt16LE(1,o+2);[s.offset,s.data.length,s.count,s.stride].forEach((v,k)=>out.writeUInt32LE(v,o+4+k*4));s.data.copy(out,s.offset);});return out;
}
module.exports={compile};
if(require.main===module){const out=path.resolve(process.argv[2]||'.cache/ayatsuri-native/cloud.aynm'),m=exportCloud(process.argv[3]||REF),bytes=compile(m);
  fs.mkdirSync(path.dirname(out),{recursive:true});fs.writeFileSync(out,bytes);fs.writeFileSync(out+'.json',JSON.stringify(m,null,2)+'\n');console.log(JSON.stringify({out,bytes:bytes.length,nodes:m.nodes.length,sha256:crypto.createHash('sha256').update(bytes).digest('hex')}));}
