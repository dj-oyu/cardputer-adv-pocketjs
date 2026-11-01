// Synthetic same-semantic occlusion control. No Blender or artistic acceptance.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {triangleFixture,makeGlb}=require('./test_import.cjs');
const {importGlb}=require('./import_glb.cjs');
const {run}=require('./pipeline.cjs');
const {compile}=require('../model/pack_model.cjs');
const {partition,sampleErrors}=require('./reduce.cjs');
const sha256=data=>crypto.createHash('sha256').update(data).digest('hex');

function overlapFixtures(){
  const source=triangleFixture({positions:[[-.75,-.5,0],[.75,-.5,0],[.75,.5,0],[-.75,.5,0]],indices:[0,1,2,0,2,3]});
  source.json.asset.generator='synthetic same-semantic near/far occlusion regression';
  source.json.nodes=[{name:'far-head',mesh:0,translation:[0,0,-.25],extras:{semanticId:'head'}},
    {name:'near-head',mesh:0,translation:[0,0,.25],extras:{semanticId:'head'}}];
  source.json.scenes[0].nodes=[0,1];
  const glb=makeGlb(source.json,source.bin),ir=importGlb(glb);
  const profile={experimental:true,assetScale:40,targetVertices:4,maxCollapses:0,
    budgets:{maxColdBytes:65536,maxArenaEstimateBytes:49152,maxSegments:1024,maxBboxPixelVisits:65535,maxNodes:8},
    quality:{maxSampleDistance:1e-6,maxNormalDegrees:.05,minSilhouetteIoU:1},
    views:[{yaw:0,pitch:0,scale:40}],
    materials:{'material/0':{albedo565:0xfc00,hone:[1,0,0,16,0,0,0,0,0,0,0]}}};
  const correct=run(ir,profile);
  const mapping=ir.surfaces.map((s,i)=>({nodeId:i+1,partId:correct.report.correspondence[i].partId,
    sourceSurfaceId:s.stableId,semanticId:'head',sourceVertices:s.sourceVertexIds.slice(),sourceTriangles:s.sourceTriangleIds.slice(),outputTriangles:structuredClone(correct.decoded[i].triangles)}));
  const metadata=(label,parts)=>({schema:'hone-pixel-fixture-v1',label,synthetic:true,
    sourceSha256:ir.source.sha256,assetScale:profile.assetScale,parts,
    expectations:{nearestSourceSurfaceId:ir.surfaces[1].stableId,semanticId:'head',
      viewDepthNear:.25,viewDepthFar:-.25,wrongOrderDepthError:.5,sourceFaceWidth:1.5,
      normalizedWrongOrderDepthError:1/3,requiresDepthOrSurfaceCorrespondence:true,
      note:'Identical albedo, normals, silhouettes and semantic IDs cannot detect reversed painter order.'}});
  correct.metadata=metadata('correct-far-then-near',mapping);
  correct.report.correspondence=structuredClone(mapping);
  // Reorder existing native nodes only. Geometry, material, source identities and
  // the immutable source IR stay identical; no alternative source is fabricated.
  const manifest=structuredClone(correct.manifest);
  manifest.nodes=[manifest.nodes[0],...manifest.nodes.slice(1).reverse().map((node,i)=>({...node,index:i+1}))];
  const bytes=compile(manifest),report=structuredClone(correct.report);
  report.binSha256=sha256(bytes);report.correspondence=mapping.slice().reverse().map((part,i)=>({...part,nodeId:i+1}));
  const reversed={bytes,manifest,report,decoded:correct.decoded.slice().reverse(),
    metadata:metadata('wrong-near-then-far',structuredClone(report.correspondence))};
  return {glb,ir,profile,cases:{correct,reversed}};
}

function sameSurfaceFixtures(){
  const base=overlapFixtures(),corners=[[-.75,-.5],[.75,-.5],[.75,.5],[-.75,.5]],
    source=triangleFixture({positions:[-.25,.25].flatMap(z=>corners.map(([x,y])=>[x,y,z])),indices:[0,1,2,0,2,3,4,5,6,4,6,7]});
  source.json.asset.generator='synthetic single-surface same-semantic occlusion regression';
  source.json.nodes[0].extras={semanticId:'head'};
  const glb=makeGlb(source.json,source.bin),ir=importGlb(glb),profile={...base.profile,targetVertices:8},baseline=run(ir,profile);
  // Deliberately use a smaller legal compiler partition budget to separate the
  // disconnected sheets while preserving their common source surface identity.
  const parts=partition(ir.surfaces[0],{maxVertices:4}),manifest=structuredClone(baseline.manifest),template=manifest.nodes[1];
  manifest.nodes=[manifest.nodes[0],...parts.map((s,i)=>({...structuredClone(template),index:i+1,name:'part-'+i,
    mesh:{...structuredClone(template.mesh),vertices:s.positions.flat().map(v=>v*profile.assetScale),
      normals:s.normals.flat(),indices:s.triangles.flat(),mask:s.positions.map(()=>-1)}}))];
  const mapping=parts.map((s,i)=>({nodeId:i+1,partId:s.stableId,sourceSurfaceId:ir.surfaces[0].stableId,semanticId:'head',
    sourceVertices:s.sourceVertexIds.slice(),sourceTriangles:s.sourceTriangleIds.slice(),outputTriangles:structuredClone(s.triangles)}));
  const report=structuredClone(baseline.report),bytes=compile(manifest);
  report.binSha256=sha256(bytes);report.coldBytes=bytes.length;report.nodes=3;report.parts=2;
  report.surfaceReports[0].parts=2;report.correspondence=mapping;
  report.quantization=parts.map(s=>sampleErrors(s,s));
  report.segmentsUpperBound=parts.reduce((sum,s)=>sum+1+Math.ceil((8+16*s.positions.length+6*s.triangles.length)/10),0);
  report.arenaConservativeEstimate=24*3+84+parts.reduce((sum,s)=>sum+Math.ceil((84+1040+12*s.positions.length)/8)*8,0);
  report.fixtureTransform={kind:'legal-partition-budget',maxVertices:4,maxTriangles:64};
  const metadata=structuredClone(base.cases.correct.metadata);
  metadata.sourceSha256=ir.source.sha256;metadata.parts=structuredClone(mapping);
  metadata.expectations.nearestSourceSurfaceId=ir.surfaces[0].stableId;
  metadata.expectations.nearestOriginalTriangles=[2,3];
  metadata.expectations.farOriginalTriangles=[0,1];
  metadata.expectations.sameSourceSurface=true;
  const correct={bytes,manifest,report,decoded:parts,metadata},reversedManifest=structuredClone(manifest);
  reversedManifest.nodes=[reversedManifest.nodes[0],...reversedManifest.nodes.slice(1).reverse().map((n,i)=>({...n,index:i+1}))];
  const reversedBytes=compile(reversedManifest),reversedReport=structuredClone(report),reversedMetadata=structuredClone(metadata);
  reversedReport.binSha256=sha256(reversedBytes);reversedReport.correspondence.reverse().forEach((p,i)=>p.nodeId=i+1);
  reversedMetadata.label='wrong-near-then-far';reversedMetadata.parts=structuredClone(reversedReport.correspondence);
  return {glb,ir,profile,cases:{correct,reversed:{bytes:reversedBytes,manifest:reversedManifest,report:reversedReport,decoded:parts.slice().reverse(),metadata:reversedMetadata}}};
}

function checkFixtureInvariants(fixture){
  const {ir,cases:{correct,reversed}}=fixture;
  assert.equal(ir.surfaces.length,2);assert.equal(correct.manifest.nodes.length,3);
  assert.notDeepEqual(correct.bytes,reversed.bytes);
  const named=nodes=>nodes.slice(1).map(({index,...node})=>node).sort((a,b)=>a.name.localeCompare(b.name));
  assert.deepEqual(named(correct.manifest.nodes),named(reversed.manifest.nodes));
  assert.deepEqual(correct.decoded[0].triangles,correct.decoded[1].triangles);
  assert.deepEqual(correct.decoded[0].normals,correct.decoded[1].normals);
  assert.deepEqual(correct.decoded[0].positions.map(p=>p.slice(0,2)),correct.decoded[1].positions.map(p=>p.slice(0,2)));
  assert(correct.decoded[0].positions.every(p=>p[2]===-.25));
  assert(correct.decoded[1].positions.every(p=>p[2]===.25));
  assert.deepEqual(ir.surfaces[0].sourceTriangleIds,ir.surfaces[1].sourceTriangleIds);
  assert.notEqual(ir.surfaces[0].stableId,ir.surfaces[1].stableId);
  for(const c of [correct,reversed]){
    assert(c.metadata.parts.every(p=>p.semanticId==='head'));
    assert.equal(c.report.binSha256,sha256(c.bytes));
    assert.equal(c.report.irHash,correct.report.irHash);
    assert(c.report.views.every(v=>v.iou===1));
  }
  assert.equal(correct.metadata.parts.at(-1).sourceSurfaceId,ir.surfaces[1].stableId);
  assert.equal(reversed.metadata.parts.at(-1).sourceSurfaceId,ir.surfaces[0].stableId);
}

function checkSameSurfaceInvariants(fixture){
  const {ir,cases:{correct,reversed}}=fixture;
  assert.equal(ir.surfaces.length,1);assert.equal(ir.surfaces[0].positions.length,8);assert.equal(ir.surfaces[0].triangles.length,4);
  assert.equal(correct.manifest.nodes.length,3);assert.notDeepEqual(correct.bytes,reversed.bytes);
  for(const c of [correct,reversed]){
    assert.equal(c.report.binSha256,sha256(c.bytes));
    assert(c.report.correspondence.every(p=>p.sourceSurfaceId===ir.surfaces[0].stableId&&p.semanticId==='head'));
    assert.deepEqual(c.report.correspondence.flatMap(p=>p.sourceTriangles).sort((a,b)=>a-b),[0,1,2,3]);
    assert.deepEqual(c.report.correspondence.flatMap(p=>p.sourceVertices).sort((a,b)=>a-b),[0,1,2,3,4,5,6,7]);
  }
  assert.deepEqual(correct.report.correspondence.at(-1).sourceTriangles,[2,3]);
  assert.deepEqual(reversed.report.correspondence.at(-1).sourceTriangles,[0,1]);
  assert(sampleErrors(ir.surfaces[0],{positions:correct.decoded.flatMap(s=>s.positions),normals:correct.decoded.flatMap(s=>s.normals),
    triangles:correct.decoded.flatMap((s,i)=>s.triangles.map(t=>t.map(v=>v+4*i)))}).distance.max<1e-12);
}

function writeFixtures(directory,fixture=overlapFixtures()){
  // An existing directory is never replaced; a partial write is diagnostic only.
  fs.mkdirSync(directory,{recursive:false});
  const write=(name,data)=>fs.writeFileSync(path.join(directory,name),data,{flag:'wx'});
  const json=(name,data)=>write(name,JSON.stringify(data,null,2)+'\n');
  write('source.glb',fixture.glb);json('scene.ir.json',fixture.ir);json('profile.json',fixture.profile);
  for(const [name,c] of Object.entries(fixture.cases)){
    write(name+'.aynm',c.bytes);json(name+'.manifest.json',c.manifest);
    json(name+'.report.json',c.report);json(name+'.metadata.json',c.metadata);
  }
  return fixture;
}

const PIXELS=240*135;
const THRESHOLDS=Object.freeze({minIoU:.97,maxColorMean:.5,maxColorP95:1,maxColorMax:2,maxDepthSource:.01,maxComponentMismatch:0,maxCorrespondenceDistanceSource:.01,depthTieEpsilonSource:1e-6});
function readNativeArtifacts(directory,expectedBin){
  return require('./oracle.cjs').loadNative(directory,sha256(expectedBin));
}
function runNativeTests(kind,correctDirectory,reversedDirectory,outputDirectory){
  assert(['two-surfaces','same-surface'].includes(kind),'FIXTURE_KIND: two-surfaces or same-surface');
  const fixture=kind==='same-surface'?sameSurfaceFixtures():overlapFixtures();
  if(kind==='same-surface')checkSameSurfaceInvariants(fixture);else checkFixtureInvariants(fixture);
  const {compare}=require('./oracle.cjs'),correct=readNativeArtifacts(correctDirectory,fixture.cases.correct.bytes),reversed=readNativeArtifacts(reversedDirectory,fixture.cases.reversed.bytes);
  const evaluate=(which,native)=>compare({sourceIR:fixture.ir,compileReport:fixture.cases[which].report,...native,thresholds:THRESHOLDS}).report;
  const good=evaluate('correct',correct),bad=evaluate('reversed',reversed);
  assert.equal(good.passed,true,'CORRECT_PAINTER_REJECTED: '+JSON.stringify(good));
  assert(good.metrics.pixelsCompared>0);assert.equal(bad.passed,false,'REVERSED_PAINTER_ACCEPTED');
  for(const key of ['coverage','diagnosticCoverage','color'])assert.equal(bad.gates[key],true,'REVERSED_'+key);
  assert.equal(bad.gates.depth,false);assert.equal(bad.gates.sourceCorrespondence,false);
  assert.equal(bad.metrics.semanticMismatch,0,'SEMANTIC_ID_MUST_NOT_REVEAL_ORDER');
  assert.deepEqual(correct.pixels,reversed.pixels,'IDENTICAL_MATERIAL_RGB_CHANGED');
  assert.deepEqual(correct.coverage,reversed.coverage,'IDENTICAL_XY_COVERAGE_CHANGED');
  assert(Math.abs(bad.metrics.depthSource.max-.5)<1e-6,'EXPECTED_HALF_UNIT_DEPTH_ERROR');
  assert(Math.abs(bad.metrics.depthSource.p95-.5)<1e-6);
  assert(bad.metrics.correspondenceSource.max>.49);
  if(kind==='same-surface'){
    assert(bad.errors.length>0);
    assert(bad.errors.every(e=>e.sourceSurfaceId===e.mappedSourceSurfaceId),'SOURCE_ID_MUST_NOT_REVEAL_ORDER');
  }
  const pixels=correct.pixels.slice();let changed=0;
  for(let i=0;i<PIXELS;i++)if(correct.coverage[i]){
    const green=(pixels[i]>>>5)&63,replacement=green<=59?green+4:green-4;
    pixels[i]=(pixels[i]&0xf81f)|(replacement<<5);changed++;
  }
  assert(changed>0);const corrupted=evaluate('correct',{...correct,pixels});
  assert.equal(corrupted.passed,false);assert.equal(corrupted.gates.color,false,'FOUR_GREEN_LEVEL_CORRUPTION_ACCEPTED');
  for(const key of ['coverage','diagnosticCoverage','depth','sourceCorrespondence'])assert.equal(corrupted.gates[key],true,'COLOR_ONLY_CHANGED_'+key);
  // A coplanar quad has identical depth and color on both triangles. Replacing
  // every covered tag with one near triangle must still fail its pixel bounds.
  const nearNodeId=fixture.cases.correct.report.correspondence.at(-1).nodeId,
    tagRecords=correct.metadata.triangleTags||correct.metadata.packets.flatMap(p=>p.triangleTags.map(tag=>({nodeId:p.nodeId,tag}))),
    nearTag=tagRecords.find(t=>t.nodeId===nearNodeId);
  assert(nearTag&&Number.isInteger(nearTag.tag)&&nearTag.tag>0&&nearTag.tag<=65535,'NEAR_TRIANGLE_TAG_MISSING');
  const tags=correct.tags.slice();let changedTags=0;
  for(let i=0;i<PIXELS;i++)if(correct.coverage[i]){if(tags[i]!==nearTag.tag)changedTags++;tags[i]=nearTag.tag;}
  assert(changedTags>0,'TAG_COLLAPSE_MUST_CHANGE_PIXELS');
  const collapsed=evaluate('correct',{...correct,tags});
  assert.equal(collapsed.passed,false,'SINGLE_TRIANGLE_TAG_COLLAPSE_ACCEPTED');
  assert.equal(collapsed.gates.diagnosticCoverage,false);assert(collapsed.metrics.tagPixelMismatch>0);
  assert.equal(collapsed.metrics.tagCoverageMismatch,0);assert.equal(collapsed.metrics.unknownTags,0);
  assert(collapsed.errors.some(e=>e.kind==='TAG_PIXEL_OUTSIDE'));
  for(const key of ['coverage','color','depth','sourceCorrespondence'])assert.equal(collapsed.gates[key],true,'TAG_ONLY_CHANGED_'+key);
  // One native-only pixel must validate its tag even outside source coverage.
  assert.equal(good.metrics.iou,1,'EXACT_COVERAGE_REQUIRED_FOR_OUTSIDE_CONTROL');
  assert(!tagRecords.some(t=>t.tag===65535),'RESERVED_UNKNOWN_TAG_IN_USE');
  const outside=correct.coverage.findIndex(v=>v===0);assert(outside>=0);assert.equal(correct.tags[outside],0);
  const outsideCoverage=correct.coverage.slice(),outsideTags=correct.tags.slice();outsideCoverage[outside]=1;outsideTags[outside]=65535;
  const unknown=evaluate('correct',{...correct,coverage:outsideCoverage,tags:outsideTags});
  assert.equal(unknown.passed,false);assert.equal(unknown.gates.diagnosticCoverage,false);
  assert.equal(unknown.metrics.unknownTags,1);assert.equal(unknown.metrics.tagCoverageMismatch,0);
  for(const key of ['coverage','color','depth','sourceCorrespondence'])assert.equal(unknown.gates[key],true,'OUTSIDE_TAG_ONLY_CHANGED_'+key);
  // A one-level error in G alone averages to 1/3 across pooled RGB samples;
  // the per-channel mean gate must reject it at the unchanged 0.5 threshold.
  const oneGreen=correct.pixels.slice();
  for(let i=0;i<PIXELS;i++)if(correct.coverage[i]){
    const green=(oneGreen[i]>>>5)&63;assert(green<63,'GREEN_INCREMENT_WOULD_CLIP');
    oneGreen[i]=(oneGreen[i]&0xf81f)|((green+1)<<5);
  }
  const channel=evaluate('correct',{...correct,pixels:oneGreen});
  assert.equal(channel.passed,false);assert.equal(channel.gates.color,false,'ONE_GREEN_LEVEL_POOLED_MEAN_ACCEPTED');
  assert(channel.metrics.colorLevels.mean<THRESHOLDS.maxColorMean);
  assert.equal(channel.metrics.colorChannels.find(c=>c.channel==='G6').mean,1);
  for(const key of ['coverage','diagnosticCoverage','depth','sourceCorrespondence'])assert.equal(channel.gates[key],true,'ONE_GREEN_ONLY_CHANGED_'+key);
  // Change only the in-memory reference: disconnected sheets at equal depth
  // are ambiguous even when their source surface and semantic IDs are equal.
  const tiedIR=structuredClone(fixture.ir),tiedReport=structuredClone(fixture.cases.correct.report);
  for(const surface of tiedIR.surfaces)for(const position of surface.positions)position[2]=.25;
  tiedReport.irHash=sha256(JSON.stringify(tiedIR));
  assert.throws(()=>compare({sourceIR:tiedIR,compileReport:tiedReport,...correct,thresholds:THRESHOLDS}),
    e=>e.message==='AMBIGUOUS_SOURCE_DEPTH','DISCONNECTED_COPLANAR_SOURCE_ACCEPTED');
  assert.equal(sha256(JSON.stringify(fixture.ir)),fixture.cases.correct.report.irHash,'SOURCE_IR_MUTATED');
  const ambiguousSourceDepth={rejectedAs:'AMBIGUOUS_SOURCE_DEPTH',referenceOnly:true,irHash:tiedReport.irHash,depthTieEpsilonSource:THRESHOLDS.depthTieEpsilonSource};
  const result={status:'PASS',fixture:kind,correct:good,reversed:bad,colorCorruption:corrupted,tagCollapse:collapsed,outsideUnknownTag:unknown,oneGreenLevel:channel,ambiguousSourceDepth,
    sourceSha256:fixture.ir.source.sha256,binSha256:{correct:sha256(fixture.cases.correct.bytes),reversed:sha256(fixture.cases.reversed.bytes)},
    nativeProof:{correct:correct.proof,reversed:reversed.proof},
    scope:'Synthetic native raster regression only; no Blender, source art, physical display or performance acceptance.'};
  if(outputDirectory){fs.mkdirSync(outputDirectory,{recursive:false});fs.writeFileSync(path.join(outputDirectory,'tests.json'),JSON.stringify(result,null,2)+'\n',{flag:'wx'});}
  return result;
}
module.exports={overlapFixtures,sameSurfaceFixtures,checkFixtureInvariants,checkSameSurfaceInvariants,writeFixtures,readNativeArtifacts,runNativeTests,THRESHOLDS};
if(require.main===module){try{
  const args=process.argv.slice(2);assert(args.length===3||args.length===4,'USAGE: fixtureKind correctNativeDir reversedNativeDir [newOutputDir]');
  const result=runNativeTests(...args);
  console.log(JSON.stringify({status:result.status,fixture:result.fixture,pixels:result.correct.metrics.pixelsCompared,
    reversedDepthMax:result.reversed.metrics.depthSource.max,colorCorruptionMax:result.colorCorruption.metrics.colorLevels.max,
    tagPixelMismatch:result.tagCollapse.metrics.tagPixelMismatch,unknownTags:result.outsideUnknownTag.metrics.unknownTags,
    greenMean:result.oneGreenLevel.metrics.colorChannels.find(c=>c.channel==='G6').mean,ambiguousSourceDepth:result.ambiguousSourceDepth.rejectedAs}));
}catch(e){console.error(JSON.stringify({status:'FAIL',error:e.message}));process.exitCode=1;}}
