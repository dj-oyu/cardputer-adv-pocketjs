'use strict';
// Editable synthetic grid only; not Blender or character source.
function fixture(size=9,bump=0){
  const positions=[],normals=[],indices=[];
  for(let y=0;y<size;y++)for(let x=0;x<size;x++){
    const u=x/(size-1)*2-1,v=y/(size-1)*2-1;positions.push(u,v,bump*(1-u*u)*(1-v*v));
    const a=2*bump*u*(1-v*v),b=2*bump*v*(1-u*u),d=Math.hypot(a,b,1);normals.push(a/d,b/d,1/d);
  }
  for(let y=0;y<size-1;y++)for(let x=0;x<size-1;x++){const a=y*size+x,b=a+1,c=a+size,d=c+1;indices.push(a,b,d,a,d,c);}
  const p=Buffer.alloc(positions.length*4),n=Buffer.alloc(normals.length*4),t=Buffer.alloc(indices.length*2);
  positions.forEach((v,i)=>p.writeFloatLE(v,i*4));normals.forEach((v,i)=>n.writeFloatLE(v,i*4));indices.forEach((v,i)=>t.writeUInt16LE(v,i*2));
  const bounds=[0,1,2].map(j=>Array.from({length:size*size},(_,i)=>p.readFloatLE((i*3+j)*4))),bin=Buffer.concat([p,n,t]),doc={asset:{version:'2.0',generator:'synthetic editable grid fixture, not Blender or fox'},scene:0,scenes:[{nodes:[0]}],nodes:[{mesh:0,name:'synthetic-grid'}],meshes:[{primitives:[{attributes:{POSITION:0,NORMAL:1},indices:2,material:0,mode:4}]}],materials:[{name:'explicit-opaque',pbrMetallicRoughness:{baseColorFactor:[1,.5,.1,1],metallicFactor:0,roughnessFactor:.8}}],buffers:[{byteLength:bin.length}],bufferViews:[{buffer:0,byteOffset:0,byteLength:p.length},{buffer:0,byteOffset:p.length,byteLength:n.length},{buffer:0,byteOffset:p.length+n.length,byteLength:t.length}],accessors:[{bufferView:0,componentType:5126,count:size*size,type:'VEC3',min:bounds.map(v=>Math.min(...v)),max:bounds.map(v=>Math.max(...v))},{bufferView:1,componentType:5126,count:size*size,type:'VEC3'},{bufferView:2,componentType:5123,count:indices.length,type:'SCALAR'}]};
  const raw=Buffer.from(JSON.stringify(doc)),json=Buffer.alloc(Math.ceil(raw.length/4)*4,32);raw.copy(json);const data=Buffer.alloc(Math.ceil(bin.length/4)*4);bin.copy(data);
  const glb=Buffer.alloc(28+json.length+data.length);glb.writeUInt32LE(0x46546c67);glb.writeUInt32LE(2,4);glb.writeUInt32LE(glb.length,8);glb.writeUInt32LE(json.length,12);glb.writeUInt32LE(0x4e4f534a,16);json.copy(glb,20);const off=20+json.length;glb.writeUInt32LE(data.length,off);glb.writeUInt32LE(0x004e4942,off+4);data.copy(glb,off+8);return {glb,editable:{positions,normals,indices}};
}

module.exports={fixture};
