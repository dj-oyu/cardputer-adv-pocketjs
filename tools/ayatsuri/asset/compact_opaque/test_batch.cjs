'use strict';
const assert=require('node:assert/strict');const{batchStaticSurfaces}=require('./batch_static.cjs');
const make=(id,x,material='orange')=>({stableId:id,materialId:material,positions:[[x,0,0],[x+1,0,0],[x,1,0]],normals:[[0,0,1],[0,0,1],[0,0,1]],triangles:[[0,1,2]]});
const input=[make('a',0),make('b',2),make('c',4,'ivory'),make('d',6)];
const scopes=new Map([['a','rigid-root'],['b','rigid-root'],['c','rigid-root'],['d','independent-jaw']]);const before=JSON.stringify(input);
const out=batchStaticSurfaces(input,{scopeBySurface:scopes});assert.equal(out.length,3);assert.equal(JSON.stringify(input),before);
assert(out.some(p=>p.scope==='rigid-root'&&p.materialId==='orange'&&p.origins.length===2));assert(out.some(p=>p.scope==='independent-jaw'&&p.origins.length===1));
const key=(p,t)=>JSON.stringify([p.scope,p.materialId,...t.map(i=>[...p.positions[i],...p.normals[i]])]);
const expected=input.flatMap(p=>p.triangles.map(t=>key({...p,scope:scopes.get(p.stableId)},t))).sort();
assert.deepEqual(out.flatMap(p=>p.triangles.map(t=>key(p,t))).sort(),expected);
assert.deepEqual(batchStaticSurfaces(input,{scopeBySurface:scopes}),out);
const many=Array.from({length:40},(_,i)=>make('part-'+i,i*2));const manyScopes=new Map(many.map(x=>[x.stableId,'static']));
const parts=batchStaticSurfaces(many,{scopeBySurface:manyScopes});assert(parts.length>1);assert(parts.every(p=>p.positions.length<=32&&p.triangles.length<=64));assert.equal(parts.reduce((n,p)=>n+p.triangles.length,0),40);
assert.throws(()=>batchStaticSurfaces(input),/EXPLICIT_STATIC_SCOPES_REQUIRED/);
assert.throws(()=>batchStaticSurfaces(input,{scopeBySurface:new Map()}),/INVALID_SURFACE_ID_OR_SCOPE/);
const bad=structuredClone(input);bad[0].positions[0][0]=NaN;assert.throws(()=>batchStaticSurfaces(bad,{scopeBySurface:scopes}),/INVALID_ATTRIBUTE/);
const alias=make('same',0);const exact=batchStaticSurfaces([make('a',0),alias],{scopeBySurface:new Map([['a','s'],['same','s']])});assert.equal(exact[0].positions.length,3);assert.equal(exact[0].triangles.length,2);
// Regression: Array.every skips holes. Attribute and index triples must be dense.
const rejectUnchanged=(surface,pattern)=>{const before=structuredClone(surface);assert.throws(()=>batchStaticSurfaces([surface],{scopeBySurface:new Map([[surface.stableId,'static']])}),pattern);assert.deepEqual(surface,before);};
for(const field of ['positions','normals'])for(let component=0;component<3;component++){
 const sparse=make('sparse',0);delete sparse[field][0][component];rejectUnchanged(sparse,/INVALID_ATTRIBUTE/);
}
const nonfiniteNormal=make('nonfinite-normal',0);nonfiniteNormal.normals[0][0]=NaN;rejectUnchanged(nonfiniteNormal,/INVALID_ATTRIBUTE/);
const sparseIndices=make('sparse-indices',0);delete sparseIndices.triangles[0][1];rejectUnchanged(sparseIndices,/INVALID_TRIANGLE/);
// Reject, never silently discard, an authored face that cannot survive exact dedup.
for(const differentNormals of [false,true]){
 const coincident=make('coincident',0);coincident.positions[1]=coincident.positions[0].slice();if(differentNormals)coincident.normals[1]=[0,1,0];rejectUnchanged(coincident,/DEGENERATE_TRIANGLE/);
}
const collinear=make('collinear',0);collinear.positions=[[0,0,0],[1,0,0],[2,0,0]];rejectUnchanged(collinear,/DEGENERATE_TRIANGLE/);
// A later invalid surface must not alter an earlier valid input either.
const lateBad=make('late-bad',0);lateBad.positions[1]=lateBad.positions[0].slice();const validFirst=make('valid-first',2);const mixed=[validFirst,lateBad],mixedBefore=structuredClone(mixed);
assert.throws(()=>batchStaticSurfaces(mixed,{scopeBySurface:new Map(mixed.map(x=>[x.stableId,'static']))}),/DEGENERATE_TRIANGLE/);assert.deepEqual(mixed,mixedBefore);
console.log('explicit-scope static batching synthetic: PASS');
