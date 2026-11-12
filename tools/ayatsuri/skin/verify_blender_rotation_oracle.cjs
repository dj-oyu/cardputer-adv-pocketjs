// Fixed public child local-X +30deg oracle; no native/adapter math imported.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const fixed=require('./verify_blender_skin_oracle.cjs');
const c=Math.cos(Math.PI/6),s=.5;
function close(actual,expected,t,label){assert.equal(actual.length,expected.length,label);for(let i=0;i<expected.length;i++)assert(Number.isFinite(actual[i])&&Math.abs(actual[i]-expected[i])<=t,`${label}[${i}]`);}
function dense(a,n){assert(Array.isArray(a)&&a.length===n);for(let i=0;i<n;i++)assert(Object.hasOwn(a,i));}
const identity=[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];
const sourceChild=[[1,0,0,15],[0,c,-s,0],[0,s,c,0],[0,0,0,1]];
const targetG=[...identity.slice(0,3).flat(),1,0,0,15,0,c,s,0,0,-s,c,0];
function analytic(w,x,y){const a=1-w+w*c,b=w*s,d=a*a+b*b;assert(d>=Math.cos(Math.PI/12)**2-1e-14,'nonsingular blend');return {position:[x,a*y,-b*y],normal:[0,b/Math.sqrt(d),a/Math.sqrt(d)],determinant:d};}
function compareGeometry(actual,expected){dense(actual,6);for(let i=0;i<6;i++){close(actual[i].position,expected[i].position,2e-5,'position');close(actual[i].normal,expected[i].normal,2e-5,'normal');}}
function verify(r,a){
 assert.equal(r.schema,'synthetic-blender-skin-rotation-v1');dense(r.samples,3);dense(a.samples,3);
 const baseline=fixed.verify({...r,schema:'synthetic-blender-skin-v1',samples:r.samples.slice(0,2)},{...a,samples:a.samples.slice(0,2)});
 const sample=r.samples[2],out=a.samples[2];assert.equal(sample.name,'child_rx30');assert.equal(out.name,sample.name);
 dense(sample.matrices,2);for(const m of sample.matrices){dense(m,4);m.forEach(row=>dense(row,4));}
 close(sample.matrices[0].flat(),identity.flat(),1e-6,'source root matrix');close(sample.matrices[1].flat(),sourceChild.flat(),1e-6,'known source local X30');
 assert(out.G instanceof Float32Array);close(Array.from(out.G),targetG,1e-6,'target reflected X rotation');
 dense(sample.positions,6);dense(sample.normals,6);
 const vertices=[];let maxBlenderPositionError=0;
 for(let i=0;i<6;i++){
  // Source geometry/weights and bind matrices were independently fixed above.
  const x=[-30,0,30][i%3],y=i<3?-15:15,w=[0,1/3,1][i%3];
  const raw=analytic(w,x,y),p=sample.positions[i];dense(p,3);const target=[p[0],-p[2],-p[1]];
  close(target,raw.position,2e-5,'evaluated Blender position');maxBlenderPositionError=Math.max(maxBlenderPositionError,...target.map((v,k)=>Math.abs(v-raw.position[k])));
  // Evaluated Blender surface normals intentionally NOT used as expected normals.
  dense(sample.normals[i],3);assert(sample.normals[i].every(Number.isFinite));
  const quantizedChild=[0,21845,65535][i%3]/65535;
  vertices.push(analytic(quantizedChild,x,y));
 }
 return {...baseline,predictions:[...baseline.predictions,{name:sample.name,vertices}],rotationMaxBlenderPositionError:maxBlenderPositionError,rotationScope:'known local-X30, identity objects; analytic fixed-weight inverse transpose, not differential surface normals'};
}
function negatives(r,a){let rejected=0;
 for(const mutation of [x=>{x.samples[2].matrices[1][1][2]*=-1;},x=>{x.samples[2].matrices[1][0][3]=0;},x=>{x.samples[2].positions.reverse();},x=>{x.samples[2].positions[2][1]+=1;},x=>{x.samples[2].matrices[0][0][0]=NaN;}]){const x=structuredClone(r);mutation(x);assert.throws(()=>verify(x,a));rejected++;}
 for(const mutation of [x=>{x.samples[2].G[18]*=-1;},x=>{x.IB[15]=0;},x=>{x.W[4]=0;},x=>{x.V[5]=-1;}]){const x=structuredClone(a);mutation(x);assert.throws(()=>verify(r,x));rejected++;}
 const expected=verify(r,a).predictions[2].vertices;
 for(const mutation of [x=>{x[1].normal[1]*=-1;},x=>{x[1].normal=[0,0,1];},x=>{x[1].normal=[0,s,c];},x=>{x[1].position[2]*=-1;}]){const x=structuredClone(expected);mutation(x);assert.throws(()=>compareGeometry(x,expected));rejected++;}
 // For spatially varying weights, source surface normals tilt differently.
 // At the middle vertex weight gradient dw/dx=1/90: normal is proportional
 // to (sin(theta)*y/90, b, a). This is not the contract's (0,b,a) fixed-weight normal.
 const x=structuredClone(expected),aMid=1-1/3+c/3,bMid=s/3;
 const surface=[s*(-15)/90,bMid,aMid],len=Math.hypot(...surface);x[1].normal=surface.map(v=>v/len);
 assert.throws(()=>compareGeometry(x,expected));rejected++;
 return rejected;
}
if(require.main===module){const [receipt,modulePath]=process.argv.slice(2);assert(receipt&&modulePath);const files=[path.resolve(receipt),path.resolve(modulePath),__filename],hash=()=>Object.fromEntries(files.map(p=>[p,crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex')]));const before=hash(),r=JSON.parse(fs.readFileSync(files[0])),a=require(files[1]),result=verify(r,a);result.negativeCases=negatives(r,a);assert.deepEqual(hash(),before);console.log(JSON.stringify({...result,inputsSha256:before},null,2));}
module.exports={verify,negatives,analytic,compareGeometry};
