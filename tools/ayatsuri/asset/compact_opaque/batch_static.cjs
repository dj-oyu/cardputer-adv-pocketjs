'use strict';
// Offline exact packing only; callers MUST supply explicit immutable transform /
// deformation scopes. This does not discover rigs or merge animated bindings.
function batchStaticSurfaces(surfaces,{scopeBySurface}={}){
 if(!Array.isArray(surfaces)||!surfaces.length||surfaces.length>256||!(scopeBySurface instanceof Map))throw Error('EXPLICIT_STATIC_SCOPES_REQUIRED');
 let vertexCount=0,triangleCount=0;const seen=new Set(),groups=new Map();
 for(const s of surfaces){
  const scope=scopeBySurface.get(s?.stableId);
  if(!s||typeof s.stableId!=='string'||!s.stableId||seen.has(s.stableId)||typeof scope!=='string'||!scope||typeof s.materialId!=='string'||!s.materialId)throw Error('INVALID_SURFACE_ID_OR_SCOPE');seen.add(s.stableId);
  if(!Array.isArray(s.positions)||s.positions.length<3||!Array.isArray(s.normals)||s.normals.length!==s.positions.length||!Array.isArray(s.triangles)||!s.triangles.length)throw Error('INVALID_SURFACE');
  vertexCount+=s.positions.length;triangleCount+=s.triangles.length;if(vertexCount>4096||triangleCount>8192)throw Error('BOUNDED_INPUT_LIMIT');
  for(const p of [...s.positions,...s.normals])if(!Array.isArray(p)||p.length!==3||!p.every(Number.isFinite))throw Error('INVALID_ATTRIBUTE');
  for(const n of s.normals)if(Math.abs(Math.hypot(...n)-1)>1e-4)throw Error('INVALID_NORMAL');
  for(const t of s.triangles)if(!Array.isArray(t)||t.length!==3||new Set(t).size!==3||!t.every(i=>Number.isInteger(i)&&i>=0&&i<s.positions.length))throw Error('INVALID_TRIANGLE');
  const key=JSON.stringify([scope,s.materialId]);let g=groups.get(key);
  if(!g){g={scope,materialId:s.materialId,positions:[],normals:[],triangles:[],origins:[],lookup:new Map()};groups.set(key,g);}
  // No tolerance, quantization or vertex relocation. Signed zero is keyed
  // separately as it can matter to downstream exact numeric transforms.
  const exact=x=>Object.is(x,-0)?'-0':x;
  const map=s.positions.map((p,i)=>{const n=s.normals[i],k=JSON.stringify([...p,...n].map(exact));let j=g.lookup.get(k);if(j===undefined){j=g.positions.length;g.lookup.set(k,j);g.positions.push(p.slice());g.normals.push(n.slice());}return j;});
  s.triangles.forEach((t,i)=>{g.triangles.push(t.map(j=>map[j]));g.origins.push({surface:s.stableId,triangle:i});});
 }
 const result=[...groups.values()].flatMap(g=>partition(g).map(p=>({...p,scope:g.scope})));
 return result;
}
function partition(g){
 const pending=new Set(g.triangles.map((_,i)=>i)),parts=[];
 while(pending.size){let ids=new Set(),tris=[],origins=[];
  while(tris.length<64){let best=-1,bestShared=-1,bestDist=Infinity;let center=[0,0,0];for(const id of ids)g.positions[id].forEach((v,k)=>center[k]+=v/ids.size);
   for(const i of pending){const t=g.triangles[i],shared=t.filter(v=>ids.has(v)).length,add=3-shared;if(ids.size+add>32)continue;
    const c=[0,1,2].map(k=>t.reduce((n,j)=>n+g.positions[j][k]/3,0));const dist=ids.size?c.reduce((n,v,k)=>n+(v-center[k])**2,0):0;
    if(shared>bestShared||(shared===bestShared&&(dist<bestDist||(dist===bestDist&&i<best)))){best=i;bestShared=shared;bestDist=dist;}
   }
   if(best<0)break;const t=g.triangles[best];tris.push(t);origins.push(g.origins[best]);t.forEach(i=>ids.add(i));pending.delete(best);
  }
  const list=[...ids],map=new Map(list.map((v,i)=>[v,i]));parts.push({positions:list.map(i=>g.positions[i]),normals:list.map(i=>g.normals[i]),triangles:tris.map(t=>t.map(i=>map.get(i))),origins,materialId:g.materialId});
 }
 return parts;
}
module.exports={batchStaticSurfaces};
