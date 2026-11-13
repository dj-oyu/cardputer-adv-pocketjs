'use strict';
// Offline provenance only. A collapse survivor is not a geometric correspondence.
function composeLineage(ir,compiled){
 if(!Array.isArray(ir?.surfaces)||!Array.isArray(compiled?.parts))throw Error('INVALID_LINEAGE_INPUT');
 const surfaces=new Map();
 for(const s of ir.surfaces){if(typeof s.stableId!=='string'||surfaces.has(s.stableId)||!Array.isArray(s.triangles)||!Array.isArray(s.sourceTriangleIds)||s.sourceTriangleIds.length!==s.triangles.length)throw Error('INVALID_SOURCE_LINEAGE');surfaces.set(s.stableId,s);}
 return Array.from(compiled.parts,(part,i)=>{
  if(!Array.isArray(part?.origins)||part.origins.length!==part.triangles?.length)throw Error('INVALID_PART_LINEAGE');
  return {nodeId:i+1,triangles:Array.from(part.origins,(origin)=>{
   const surface=surfaces.get(origin?.surface),index=origin?.triangle;
   if(!surface||!Number.isSafeInteger(index)||index<0||index>=surface.triangles.length)throw Error('INVALID_TRIANGLE_ORIGIN');
   const sourceTriangleId=surface.sourceTriangleIds[index];
   if(!Number.isSafeInteger(sourceTriangleId)||sourceTriangleId<0)throw Error('INVALID_SOURCE_TRIANGLE');
   return {surfaceId:surface.stableId,reducedTriangle:index,sourceTriangleId};
  })};
 });
}
module.exports={composeLineage};
