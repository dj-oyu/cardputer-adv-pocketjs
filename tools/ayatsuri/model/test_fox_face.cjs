'use strict';
const assert=require('node:assert/strict'),crypto=require('node:crypto');
const {exportFoxFace,generate}=require('./export_fox_face.cjs'),{compile}=require('./pack_model.cjs');
const m=exportFoxFace(),a=compile(m),b=compile(exportFoxFace());
assert.ok(a.equals(b));assert.equal(m.nodes.length,25);
assert.deepEqual(m.nodes.map(n=>n.parent),[-1,0,0,0,0,4,0,0,0,0,9,9,9,9,9,9,0,16,16,16,16,16,16,0,0]);
assert.deepEqual(m.nodes.filter(n=>n.kind==='group').map(n=>n.index),[0,4,9,16]);
assert.deepEqual(m.nodes.filter(n=>!n.enabled).map(n=>n.index),[7]);
assert.deepEqual(m.nodes[7].pose,[0,15,24,0,0,0,1,Math.fround(.15),1]);
assert.deepEqual(m.clips.map(a=>[a.anchor,a.targets,a.vertices.length/3,a.empty]),[[2,[2],3,null],[3,[3],3,null],[10,[10,11,12],8,null],[17,[17,18,19],8,null]]);
assert.equal(m.nodes.filter(n=>n.kind==='blendEllipsoids').reduce((sum,n)=>sum+n.lobes.length/6,0),13);
assert.equal(a.length,3148);
assert.equal(crypto.createHash('sha256').update(a).digest('hex'),'3c296648c459ff0cc1b03849febfbd93f96e8cf64345ede2c28e8f7bde691888');
// Removing offline clip metadata must not change the existing AYNM encoding.
const bare={...m};delete bare.clips;assert.ok(compile(bare).equals(a));
console.log('FOX_FACE_EXPORT_PASS',JSON.stringify(generate()));
