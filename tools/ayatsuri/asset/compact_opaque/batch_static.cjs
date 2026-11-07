'use strict';
// Offline exact packing only; callers MUST supply explicit immutable transform /
// deformation scopes. This does not discover rigs or merge animated bindings.
// Never use Array.every directly on caller triples: it skips sparse holes.
const denseTriple=(a,valid)=>Array.isArray(a)&&a.length===3&&[0,1,2].every(k=>Object.prototype.hasOwnProperty.call(a,k)&&valid(a[k]));
function finiteArea(positions,t){
 const [a,b,c]=t.map(i=>positions[i]),u=b.map((x,k)=>x-a[k]),v=c.map((x,k)=>x-a[k]);
 const n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
 return n.every(Number.isFinite)&&n.some(x=>x!==0);
}
function batchStaticSurfaces(surfaces,{scopeBySurface,preserveOrder=false}={}){
 if(typeof preserveOrder!=='boolean')throw Error('INVALID_ORDER_POLICY');
 if(!Array.isArray(surfaces)||!surfaces.length||surfaces.length>256||!(scopeBySurface instanceof Map))throw Error('EXPLICIT_STATIC_SCOPES_REQUIRED');
 let vertexCount=0,triangleCount=0;const seen=new Set(),groups=new Map();
 for(const s of surfaces){
  const scope=scopeBySurface.get(s?.stableId);
  if(!s||typeof s.stableId!=='string'||!s.stableId||seen.has(s.stableId)||typeof scope!=='string'||!scope||typeof s.materialId!=='string'||!s.materialId)throw Error('INVALID_SURFACE_ID_OR_SCOPE');seen.add(s.stableId);
  if(!Array.isArray(s.positions)||s.positions.length<3||!Array.isArray(s.normals)||s.normals.length!==s.positions.length||!Array.isArray(s.triangles)||!s.triangles.length)throw Error('INVALID_SURFACE');
  vertexCount+=s.positions.length;triangleCount+=s.triangles.length;if(vertexCount>4096||triangleCount>8192)throw Error('BOUNDED_INPUT_LIMIT');
  for(const p of [...s.positions,...s.normals])if(!denseTriple(p,Number.isFinite))throw Error('INVALID_ATTRIBUTE');
  for(const n of s.normals){const length=Math.hypot(...n);if(!Number.isFinite(length)||Math.abs(length-1)>1e-4)throw Error('INVALID_NORMAL');}
  for(const t of s.triangles){
   if(!denseTriple(t,i=>Number.isInteger(i)&&i>=0&&i<s.positions.length)||new Set(t).size!==3)throw Error('INVALID_TRIANGLE');
   if(!finiteArea(s.positions,t))throw Error('DEGENERATE_TRIANGLE');
  }
  const side=s.materialSidedness??'single';
  if(side!=='single'&&side!=='double')throw Error('INVALID_SIDEDNESS');
  const groupKey=JSON.stringify([scope,s.materialId,side]);
  // Ordered mode only merges adjacent compatible surfaces, never moving an
  // intervening surface/face behind a different material or side policy.
  const last=[...groups.values()].at(-1);
  const key=preserveOrder?(last&&last.groupKey===groupKey?last.key:String(groups.size)):groupKey;
  let g=groups.get(key);
  if(!g){g={key,groupKey,scope,side,explicit:s.materialSidedness!==undefined,materialId:s.materialId,positions:[],normals:[],triangles:[],origins:[],lookup:new Map()};groups.set(key,g);}
  if(s.materialSidedness!==undefined)g.explicit=true;
  // No tolerance, quantization or vertex relocation. Signed zero is keyed
  // separately as it can matter to downstream exact numeric transforms.
  const exact=x=>Object.is(x,-0)?'-0':x;
  const map=s.positions.map((p,i)=>{const n=s.normals[i],k=JSON.stringify([...p,...n].map(exact));let j=g.lookup.get(k);if(j===undefined){j=g.positions.length;g.lookup.set(k,j);g.positions.push(p.slice());g.normals.push(n.slice());}return j;});
  s.triangles.forEach((t,i)=>{const mapped=t.map(j=>map[j]);if(new Set(mapped).size!==3)throw Error('DEGENERATE_TRIANGLE');g.triangles.push(mapped);g.origins.push({surface:s.stableId,triangle:i});});
 }
 const result=[...groups.values()].flatMap(g=>partition(g,preserveOrder).map(p=>({...p,scope:g.scope,...(g.explicit?{materialSidedness:g.side}:{})})));
 return result;
}
function partition(g,preserveOrder){
 const pending=new Set(g.triangles.map((_,i)=>i)),parts=[];
 while(pending.size){let ids=new Set(),tris=[],origins=[];
  while(tris.length<64){let best=-1,bestShared=-1,bestDist=Infinity;let center=[0,0,0];for(const id of ids)g.positions[id].forEach((v,k)=>center[k]+=v/ids.size);
   for(const i of pending){const t=g.triangles[i],shared=t.filter(v=>ids.has(v)).length,add=3-shared;
    if(preserveOrder){if(ids.size+add<=32)best=i;break;}
    if(ids.size+add>32)continue;
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
