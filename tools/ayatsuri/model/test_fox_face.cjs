'use strict';
const assert=require('node:assert/strict'),crypto=require('node:crypto'),fs=require('node:fs'),path=require('node:path');
const {exportFoxFace,generate}=require('./export_fox_face.cjs'),{compile}=require('./pack_model.cjs');
const m=exportFoxFace(),a=compile(m),b=compile(exportFoxFace());
assert.ok(a.equals(b));assert.equal(m.nodes.length,29);
assert.deepEqual(m.nodes.map(n=>n.parent),[-1,0,0,0,0,4,0,0,0,0,9,9,9,9,9,9,0,16,16,16,16,16,16,0,0,2,2,3,3]);
assert.deepEqual(m.nodes.filter(n=>n.kind==='group').map(n=>n.index),[0,4,9,16]);
assert.deepEqual(m.nodes.filter(n=>!n.enabled).map(n=>n.index),[7]);
assert.deepEqual(m.nodes[7].pose,[0,15,22,0,0,0,1,Math.fround(.15),1]);
assert.deepEqual(m.clips.map(a=>[a.anchor,a.targets,a.vertices.length/3,a.empty]),[[2,[2],5,null],[3,[3],5,null],[10,[10,11,12],8,null],[17,[17,18,19],8,null],[25,[25],5,null],[26,[26],5,null],[27,[27],5,null],[28,[28],5,null]]);
assert.equal(m.nodes.filter(n=>n.kind==='blendEllipsoids').reduce((sum,n)=>sum+n.lobes.length/6,0),13);
assert.equal(a.length,3620);
assert.equal(crypto.createHash('sha256').update(a).digest('hex'),'6bb3214811d25d61349a2bd34078adc5f50a0fa047717e94f5523eb5a04f93e3');
// Preserve the old authoring source as an immutable visual/contract baseline.
const old=fs.readFileSync(path.join(__dirname,'fixtures/fox_face_25.js'),'utf8').replace(/\r\n/g,'\n');
assert.equal(crypto.createHash('sha256').update(old).digest('hex'),'ee4dfdddf14b88da8d97bf8c6758644de89fc4e7f343ff15372c926e19039ffa');
assert.deepEqual(m.nodes[10].radii,[Math.fround(8.4),8.5,Math.fround(1.1)]);
const v2=fs.readFileSync(path.join(__dirname,'fixtures/fox_face_v2.js'),'utf8').replace(/\r\n/g,'\n');
assert.equal(crypto.createHash('sha256').update(v2).digest('hex'),'84829228c40a45021f77fd839e42698828ba89f515750a2a30861a3d68abcb33');
assert.deepEqual(m.nodes[11].radii,[Math.fround(5.8),Math.fround(6.2),Math.fround(.55)]);
assert.equal(m.nodes[11].pose[2],1);assert.equal(m.nodes[12].pose[2],1.5);
assert.deepEqual(m.nodes[9].pose.slice(0,3),[-11.5,-4.5,11]);
assert.equal(m.nodes[25].color,0xe98974);assert.equal(m.nodes[25].thickness,.12);
assert.ok(m.nodes[2].radii[0]>m.nodes[25].radii[0]);
// Removing offline clip metadata must not change the existing AYNM encoding.
const bare={...m};delete bare.clips;assert.ok(compile(bare).equals(a));
console.log('FOX_FACE_EXPORT_PASS',JSON.stringify(generate()));
