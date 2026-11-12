"use strict";
const fs=require('fs'),path=require('path'),assert=require('node:assert/strict');
const dense=a=>Array.isArray(a)&&Array.from({length:a.length},(_,i)=>Object.hasOwn(a,i)).every(Boolean);
const base=require('./compile_blender_skin_fixture.cjs');
function build(r){
 assert.equal(r.schema,'synthetic-blender-skin-rotation-v1');
 assert.deepEqual(r.samples.map(s=>s.name),['rest','child_y6','child_rx30']);
 const a=base.build({...r,schema:'synthetic-blender-skin-v1',samples:r.samples.slice(0,2)}),s=r.samples[2],c=Math.cos(Math.PI/6),h=Math.sin(Math.PI/6);
 const expected=[[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],[[1,0,0,15],[0,c,-h,0],[0,h,c,0],[0,0,0,1]]];
 assert(dense(s.matrices)&&s.matrices.length===2);
 s.matrices.forEach((m,j)=>{assert(dense(m)&&m.length===4);m.forEach((row,i)=>{assert(dense(row)&&row.length===4);row.forEach((x,k)=>assert(Number.isFinite(x)&&Math.abs(x-expected[j][i][k])<1e-6));});});
 for(const key of ['positions','normals']){assert(dense(s[key])&&s[key].length===6);for(const v of s[key])assert(dense(v)&&v.length===3&&v.every(Number.isFinite));}
 for(const n of s.normals)assert(Math.abs(Math.hypot(...n)-1)<1e-5);
 // Basis B=[X,-Z,-Y] is a reflection. B Rx(+30) B^-1 = Rx(-30).
 const B=[[1,0,0],[0,0,-1],[0,-1,0]],G=[];for(const m of s.matrices)for(let i=0;i<3;i++){for(let j=0;j<3;j++)G.push(B[i].reduce((sum,x,k)=>sum+x*B[j].reduce((v,y,l)=>v+m[k][l]*y,0),0));G.push(B[i].reduce((v,x,k)=>v+x*m[k][3],0));}
 a.samples.push({name:s.name,G:new Float32Array(G)});return a;
}
module.exports={build};
if(require.main===module){const a=build(JSON.parse(fs.readFileSync(process.argv[2]))),out=process.argv[3];fs.mkdirSync(out,{recursive:false});fs.writeFileSync(path.join(out,'model.aynm'),a.model);const arr=(n,v)=>`${n}:new ${v.constructor.name}(${JSON.stringify([...v])})`;fs.writeFileSync(path.join(out,'arrays.cjs'),`module.exports={${['V','J','W','IB'].map(n=>arr(n,a[n])).join(',')},samples:[${a.samples.map(s=>`{name:${JSON.stringify(s.name)},${arr('G',s.G)}}`).join(',')}]};\n`);}
