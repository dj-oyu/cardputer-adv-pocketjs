// Independent test-only oracle. No importer, compiler or skin implementation imports.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const I=()=>[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];
const B=[[1,0,0,0],[0,0,-1,0],[0,-1,0,0],[0,0,0,1]];
function dense(a,n){assert(Array.isArray(a)&&a.length===n);for(let i=0;i<n;i++)assert(Object.hasOwn(a,i));}
function vec(a,n){dense(a,n);a.forEach(x=>assert(Number.isFinite(x)));return a;}
function mat(a){dense(a,4);a.forEach(r=>vec(r,4));assert.deepEqual(a[3],[0,0,0,1]);return a;}
function mul(a,b){return a.map(r=>b[0].map((_,j)=>r.reduce((s,x,k)=>s+x*b[k][j],0)));}
function inverse(m){const a=m.map((r,i)=>[...r,...I()[i]]);for(let j=0;j<4;j++){let p=j;for(let i=j+1;i<4;i++)if(Math.abs(a[i][j])>Math.abs(a[p][j]))p=i;assert(Math.abs(a[p][j])>1e-10,'singular matrix');[a[j],a[p]]=[a[p],a[j]];const d=a[j][j];a[j]=a[j].map(x=>x/d);for(let i=0;i<4;i++)if(i!==j){const f=a[i][j];a[i]=a[i].map((x,k)=>x-f*a[j][k]);}}return a.map(r=>r.slice(4));}
const cv=m=>mul(mul(B,m),B),point=(m,v)=>m.slice(0,3).map(r=>r[3]+r.slice(0,3).reduce((s,x,i)=>s+x*v[i],0));
const basis=v=>[v[0],-v[2],-v[1]],flat=m=>m.slice(0,3).flat();
function close(a,b,t,label){assert.equal(a.length,b.length,label);a.forEach((x,i)=>assert(Number.isFinite(x)&&Math.abs(x-b[i])<=t,`${label}[${i}]: ${x} != ${b[i]}`));}
function quantize(w){vec(w,2);assert(w.every(x=>x>=0)&&w[0]+w[1]>0);const q=w.map(x=>65535*x/(w[0]+w[1])),out=q.map(Math.floor);let left=65535-out[0]-out[1];const order=[0,1].sort((a,b)=>(q[b]-out[b])-(q[a]-out[a])||a-b);for(let i=0;i<left;i++)out[order[i]]++;return out;}
function typed(a,T,n){assert(a instanceof T);assert.equal(a.length,n);}
function verify(r,a){
 assert.equal(r.schema,'synthetic-blender-skin-v1');dense(r.vertices,6);dense(r.normals,6);dense(r.weights,6);dense(r.faces,4);dense(r.restMatrices,2);r.restMatrices.forEach(mat);
 const expected=[[-30,0,15],[0,0,15],[30,0,15],[-30,0,-15],[0,0,-15],[30,0,-15]];
 r.vertices.forEach((v,i)=>{vec(v,3);close(v,expected[i],1e-6,'authored vertex');vec(r.normals[i],3);assert(Math.abs(Math.hypot(...r.normals[i])-1)<1e-5);});
 // B is a reflection: these source corners must become the known positive-Z strip.
 assert.deepEqual(r.faces.map(([a,b,c])=>[a,c,b]),[[0,1,4],[0,4,3],[1,2,5],[1,5,4]]);
 typed(a.V,Float32Array,36);typed(a.J,Uint16Array,24);typed(a.W,Uint16Array,24);typed(a.IB,Float32Array,24);
 const sourceChild=I();sourceChild[0][3]=15;close(r.restMatrices[0].flat(),I().flat(),1e-7,'authored root bind');close(r.restMatrices[1].flat(),sourceChild.flat(),1e-7,'authored child bind');
 const rest=r.restMatrices.map(cv),ib=rest.map(inverse);close(Array.from(a.IB),ib.flatMap(flat),1e-5,'inverse bind');
 rest.forEach((m,i)=>close(mul(m,ib[i]).flat(),I().flat(),1e-9,'rest identity'));
 const quant=[];
 for(let i=0;i<6;i++){close(Array.from(a.V.slice(i*6,i*6+6)),[...basis(r.vertices[i]),...basis(r.normals[i])],1e-5,'bind geometry');const q=quantize(r.weights[i]);close(r.weights[i],[1-[0,1/3,1][i%3],[0,1/3,1][i%3]],1e-6,'authored weight');quant.push(q);const accum=[0,0];let total=0;for(let s=0;s<4;s++){const j=a.J[i*4+s],w=a.W[i*4+s];assert(j<2,'zero slots also need valid joints');accum[j]+=w;total+=w;}assert.equal(total,65535);assert.deepEqual(accum,q,'source-ID weight mapping');}
 assert(Array.isArray(r.samples)&&r.samples.length>=2);assert(Array.isArray(a.samples)&&a.samples.length===r.samples.length);const names=new Set();let maxBlenderPositionError=0;const predictions=[];
 for(let s=0;s<r.samples.length;s++){
  const sample=r.samples[s],out=a.samples[s];assert.equal(out.name,sample.name);assert(!names.has(sample.name));names.add(sample.name);dense(sample.matrices,2);sample.matrices.forEach(mat);dense(sample.positions,6);
  assert(['rest','child_y6'].includes(sample.name),'known pose only');const expectedChild=sourceChild.map(r=>r.slice());if(sample.name==='child_y6')expectedChild[1][3]=6;close(sample.matrices[0].flat(),I().flat(),1e-7,'authored root pose');close(sample.matrices[1].flat(),expectedChild.flat(),1e-7,'authored child pose');
  const globals=sample.matrices.map(cv);typed(out.G,Float32Array,24);close(Array.from(out.G),globals.flatMap(flat),1e-5,'pose global');const palettes=globals.map((g,j)=>mul(g,ib[j])),vertices=[];
  for(let v=0;v<6;v++){
   const raw=r.weights[v],sum=raw[0]+raw[1],w=raw.map(x=>x/sum),p=basis(r.vertices[v]),candidate=palettes.map(m=>point(m,p)),expectedPos=p.map((_,k)=>w[0]*candidate[0][k]+w[1]*candidate[1][k]);
   vec(sample.positions[v],3);const actual=basis(sample.positions[v]);close(actual,expectedPos,1e-4,'Blender evaluated position');maxBlenderPositionError=Math.max(maxBlenderPositionError,...actual.map((x,k)=>Math.abs(x-expectedPos[k])));
   const qw=quant[v].map(x=>x/65535),mix=I();for(let i=0;i<3;i++)for(let j=0;j<4;j++)mix[i][j]=qw[0]*palettes[0][i][j]+qw[1]*palettes[1][i][j];const inv=inverse(mix),n=basis(r.normals[v]),normal=[0,1,2].map(i=>[0,1,2].reduce((sum,j)=>sum+inv[j][i]*n[j],0)),length=Math.hypot(...normal);assert(length>0);vertices.push({position:point(mix,p),normal:normal.map(x=>x/length)});
  }predictions.push({name:sample.name,vertices});
 }
 return {accepted:'receipt and generated input arrays only; not runtime skin/native pixels',maxBlenderPositionError,predictions,normalScope:'fixed-weight blended inverse-transpose predictions; evaluated Blender surface normals are not equivalent'};
}
function mutations(r,a){let rejected=0;const clone=()=>({V:new Float32Array(a.V),J:new Uint16Array(a.J),W:new Uint16Array(a.W),IB:new Float32Array(a.IB),samples:a.samples.map(s=>({name:s.name,G:new Float32Array(s.G)}))});const bad=[b=>{b.V[0]+=1;},b=>{b.V[1]=1;},b=>{b.IB[15]+=1;},b=>{[b.W[0],b.W[8]]=[b.W[8],b.W[0]];},b=>{b.J[3]=2;},b=>{b.samples[1].G[3]+=1;},b=>{b.V[5]=NaN;}];for(const mutate of bad){const b=clone();mutate(b);assert.throws(()=>verify(r,b),'mutation must reject');rejected++;}const receiptBad=[x=>{x.vertices.reverse();},x=>{x.weights[1].reverse();},x=>{x.restMatrices[1][0][3]=14;},x=>{x.samples[1].positions[0][0]+=1;},x=>{x.faces[0].reverse();},x=>{x.normals[0][0]=NaN;}];for(const mutate of receiptBad){const x=JSON.parse(JSON.stringify(r));mutate(x);assert.throws(()=>verify(x,a),'receipt mutation must reject');rejected++;}return rejected;}
if(require.main===module){const [receipt,modulePath]=process.argv.slice(2);assert(receipt&&modulePath,'receipt.json arrays.cjs required');const paths=[path.resolve(receipt),path.resolve(modulePath)],hashes=()=>Object.fromEntries(paths.map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex')]));const before=hashes(),r=JSON.parse(fs.readFileSync(paths[0])),a=require(paths[1]),result=verify(r,a);result.negativeCases=mutations(r,a);assert.deepEqual(hashes(),before,'input bytes unchanged');console.log(JSON.stringify({...result,inputsSha256:before},null,2));}
module.exports={verify,mutations,quantize};
