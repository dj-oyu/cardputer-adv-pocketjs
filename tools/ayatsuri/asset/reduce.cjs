// Offline static endpoint-QEM prototype. Runtime geometry is never edited here.
'use strict';
const sub=(a,b)=>a.map((x,i)=>x-b[i]);
const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const length=a=>Math.hypot(...a);
const unit=a=>{const n=length(a);if(!(n>0))throw Error('DEGENERATE');return a.map(x=>x/n);};
function fail(code){const e=Error(code);e.code=code;throw e;}
const denseTriple=(a,valid)=>Array.isArray(a)&&a.length===3&&[0,1,2].every(k=>Object.hasOwn(a,k)&&valid(a[k]));
function validate(s){
  if(!s||!Array.isArray(s.positions)||s.positions.length<3||!Array.isArray(s.normals)||s.normals.length!==s.positions.length||!Array.isArray(s.triangles)||!s.triangles.length)fail('INVALID_SURFACE');
  if(s.positions.length>4096||s.triangles.length>8192)fail('PROTOTYPE_INPUT_LIMIT');
  for(const [field,count] of [['sourceVertexIds',s.positions.length],['sourceTriangleIds',s.triangles.length]])if(s[field]!==undefined&&(!Array.isArray(s[field])||s[field].length!==count||Array.from(s[field]).some(x=>!Number.isSafeInteger(x)||x<0)))fail('INVALID_LINEAGE');
  for(const p of s.positions)if(!denseTriple(p,Number.isFinite))fail('INVALID_POSITION');
  for(const n of s.normals)if(!denseTriple(n,Number.isFinite)||Math.abs(length(n)-1)>1e-5)fail('INVALID_NORMAL');
  for(const t of s.triangles){if(!denseTriple(t,i=>Number.isInteger(i)&&i>=0&&i<s.positions.length)||new Set(t).size!==3)fail('INVALID_INDEX');
    const area=length(cross(sub(s.positions[t[1]],s.positions[t[0]]),sub(s.positions[t[2]],s.positions[t[0]])));
    if(!Number.isFinite(area)||area<=1e-14)fail('DEGENERATE');}
}
function edgeMap(triangles){
  const map=new Map();
  for(const t of triangles)for(let j=0;j<3;j++){const a=Math.min(t[j],t[(j+1)%3]),b=Math.max(t[j],t[(j+1)%3]),key=a+':'+b;
    if(!map.has(key))map.set(key,{a,b,count:0,direction:0});map.get(key).count++;map.get(key).direction+=t[j]===a?1:-1;}
  return [...map.values()].sort((a,b)=>a.a-b.a||a.b-b.b);
}
function compact(s,triangles,sourceIds){
  const ids=[...new Set(triangles.flat())].sort((a,b)=>a-b),map=new Map(ids.map((x,i)=>[x,i]));
  return {...s,positions:ids.map(i=>s.positions[i].slice()),normals:ids.map(i=>s.normals[i].slice()),
    triangles:triangles.map(t=>t.map(i=>map.get(i))),sourceVertexIds:ids.map(i=>sourceIds[i]),
    sourceCorrespondence:ids.map(i=>({sourceVertex:sourceIds[i]}))};
}
function reduceSurface(surface,{targetVertices,maxCollapses=10000,maxTriangleTests=2000000,protectedVertices=[]}={}){
  validate(surface);
  if(!Number.isInteger(targetVertices)||targetVertices<3||!Number.isInteger(maxCollapses)||maxCollapses<0||!Number.isSafeInteger(maxTriangleTests)||maxTriangleTests<1)fail('INVALID_REDUCTION_PROFILE');
  if(surface.positions.length>4096||surface.triangles.length>8192)fail('PROTOTYPE_INPUT_LIMIT');
  if(!Array.isArray(protectedVertices))fail('INVALID_PROTECTED_VERTEX');
  const s=structuredClone(surface),sourceIds=s.sourceVertexIds||s.positions.map((_,i)=>i),q=s.positions.map(()=>Array(16).fill(0)),locked=new Set(protectedVertices);
  if([...locked].some(i=>!Number.isInteger(i)||i<0||i>=s.positions.length))fail('INVALID_PROTECTED_VERTEX');
  const initialEdges=edgeMap(s.triangles);
  if(initialEdges.some(e=>e.count>2))fail('NONMANIFOLD');
  if(initialEdges.some(e=>e.count===2&&e.direction!==0))fail('INCONSISTENT_WINDING');
  // Keep every boundary sample fixed; interior vertices may collapse onto it.
  for(const e of initialEdges)if(e.count===1){locked.add(e.a);locked.add(e.b);}
  for(const t of s.triangles){const n=unit(cross(sub(s.positions[t[1]],s.positions[t[0]]),sub(s.positions[t[2]],s.positions[t[0]]))),p=[...n,-dot(n,s.positions[t[0]])];
    for(const id of t)for(let i=0;i<4;i++)for(let j=0;j<4;j++)q[id][i*4+j]+=p[i]*p[j];}
  let triangles=s.triangles,triangleIds=s.sourceTriangleIds||s.triangles.map((_,i)=>i),collapses=0,triangleTests=0;
  while(new Set(triangles.flat()).size>targetVertices){
    if(collapses===maxCollapses)fail('SEARCH_BUDGET_EXCEEDED');
    const candidates=[];
    for(const e of edgeMap(triangles)){
      if(locked.has(e.a)&&locked.has(e.b))continue;
      for(const [keep,drop] of [[e.a,e.b],[e.b,e.a]]){
        if(locked.has(drop))continue;
        const p=[...s.positions[keep],1],quad=q[keep].map((v,i)=>v+q[drop][i]);
        let cost=0;for(let i=0;i<4;i++)for(let j=0;j<4;j++)cost+=p[i]*quad[i*4+j]*p[j];
        if(!Number.isFinite(cost)||!quad.every(Number.isFinite))fail('NUMERIC_RANGE');
        candidates.push({keep,drop,cost:Math.max(0,cost),quad});
      }
    }
    candidates.sort((a,b)=>a.cost-b.cost||a.keep-b.keep||a.drop-b.drop);
    let accepted=false;
    for(const c of candidates){
      const next=[],nextIds=[],keys=new Set();let good=true;
      for(const [index,t] of triangles.entries()){
        if(++triangleTests>maxTriangleTests)fail('SEARCH_BUDGET_EXCEEDED');
        const r=t.map(i=>i===c.drop?c.keep:i);if(new Set(r).size!==3)continue;
        const oldN=cross(sub(s.positions[t[1]],s.positions[t[0]]),sub(s.positions[t[2]],s.positions[t[0]])),newN=cross(sub(s.positions[r[1]],s.positions[r[0]]),sub(s.positions[r[2]],s.positions[r[0]]));
        const key=r.slice().sort((a,b)=>a-b).join(':');
        if(length(newN)<=1e-14||dot(oldN,newN)<=0||keys.has(key)){good=false;break;}
        keys.add(key);next.push(r);nextIds.push(triangleIds[index]);
      }
      if(!good||!next.length||edgeMap(next).some(e=>e.count>2))continue;
      // The link condition preserves manifold connectivity for edge collapse.
      const neighbours=id=>new Set(triangles.filter(t=>t.includes(id)).flat().filter(x=>x!==id));
      const a=neighbours(c.keep),b=neighbours(c.drop),common=[...a].filter(x=>b.has(x));
      const opposites=new Set(triangles.filter(t=>t.includes(c.keep)&&t.includes(c.drop)).flat().filter(x=>x!==c.keep&&x!==c.drop));
      if(common.length!==opposites.size||common.some(x=>!opposites.has(x)))continue;
      triangles=next;triangleIds=nextIds;q[c.keep]=c.quad;collapses++;accepted=true;break;
    }
    if(!accepted)fail('REDUCTION_CONSTRAINTS');
  }
  const result=compact(s,triangles,sourceIds);result.sourceTriangleIds=triangleIds;validate(result);
  return {surface:result,stats:{inputVertices:s.positions.length,outputVertices:result.positions.length,inputTriangles:s.triangles.length,outputTriangles:triangles.length,collapses,triangleTests,method:'endpoint-qem-protected-boundary-v1'}};
}
function partition(surface,{maxVertices=32,maxTriangles=64}={}){
  validate(surface);if(!Number.isInteger(maxVertices)||maxVertices<3||maxVertices>32||!Number.isInteger(maxTriangles)||maxTriangles<1||maxTriangles>64)fail('INVALID_PARTITION_PROFILE');
  const parts=[];let tris=[],ids=new Set();
  const flush=()=>{if(!tris.length)return;const p=compact(surface,tris,surface.sourceVertexIds||surface.positions.map((_,i)=>i));
    p.sourceSurfaceId=surface.stableId;p.semanticId=surface.semanticId||surface.extras?.semanticId||surface.stableId;
    p.stableId=surface.stableId+'/part/'+parts.length;p.sourceTriangleIds=tris.map(t=>{const i=surface.triangles.indexOf(t);return surface.sourceTriangleIds?.[i]??i;});parts.push(p);tris=[];ids=new Set();};
  for(const t of surface.triangles){if(tris.length===maxTriangles||new Set([...ids,...t]).size>maxVertices)flush();tris.push(t);for(const i of t)ids.add(i);}
  flush();return parts;
}
// Closest point and barycentrics, including triangle edge regions.
function closest(p,a,b,c){
  const ab=sub(b,a),ac=sub(c,a),ap=sub(p,a),d1=dot(ab,ap),d2=dot(ac,ap);
  let w;
  if(d1<=0&&d2<=0)w=[1,0,0];else{
    const bp=sub(p,b),d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0&&d4<=d3)w=[0,1,0];else{
      const vc=d1*d4-d3*d2;
      if(vc<=0&&d1>=0&&d3<=0){const v=d1/(d1-d3);w=[1-v,v,0];}else{
        const cp=sub(p,c),d5=dot(ab,cp),d6=dot(ac,cp);
        if(d6>=0&&d5<=d6)w=[0,0,1];else{
          const vb=d5*d2-d1*d6;
          if(vb<=0&&d2>=0&&d6<=0){const z=d2/(d2-d6);w=[1-z,0,z];}else{
            const va=d3*d6-d5*d4;
            if(va<=0&&d4-d3>=0&&d5-d6>=0){const z=(d4-d3)/(d4-d3+d5-d6);w=[0,1-z,z];}
            else{const den=1/(va+vb+vc),v=vb*den,z=vc*den;w=[1-v-z,v,z];}
          }
        }
      }
    }
  }
  const point=a.map((_,i)=>w[0]*a[i]+w[1]*b[i]+w[2]*c[i]);return {point,w,distance:length(sub(p,point))};
}
function sampleErrors(source,reduced){
  validate(source);validate(reduced);
  if((source.positions.length+source.triangles.length)*reduced.triangles.length+(reduced.positions.length+reduced.triangles.length)*source.triangles.length>2000000)fail('VERIFICATION_BUDGET_EXCEEDED');
  const distances=[],angles=[];
  for(const [a,b] of [[source,reduced],[reduced,source]]){
    const samples=a.positions.map((p,i)=>({p,n:a.normals[i]}));
    for(const t of a.triangles)samples.push({p:[0,1,2].map(j=>t.reduce((v,i)=>v+a.positions[i][j]/3,0)),n:unit([0,1,2].map(j=>t.reduce((v,i)=>v+a.normals[i][j],0)))});
    for(const {p,n} of samples){let best;
      for(const t of b.triangles){const r=closest(p,...t.map(i=>b.positions[i]));if(!best||r.distance<best.distance){const bn=[0,1,2].map(j=>t.reduce((v,id,k)=>v+r.w[k]*b.normals[id][j],0));best={...r,n:unit(bn)};}}
      // F32 unit normals have rounded lengths; compare directions, not magnitudes.
      const normal=unit(n);distances.push(best.distance);angles.push(Math.atan2(length(cross(normal,best.n)),dot(normal,best.n))*180/Math.PI);
    }
  }
  const stats=xs=>{const sorted=xs.slice().sort((a,b)=>a-b);return {max:sorted.at(-1),p95:sorted[Math.ceil(sorted.length*.95)-1],mean:xs.reduce((a,b)=>a+b,0)/xs.length};};
  return {sampling:'bidirectional vertices and face centroids; not continuous maximum',samples:distances.length,distance:stats(distances),normalDegrees:stats(angles)};
}
module.exports={reduceSurface,partition,sampleErrors,validate};
