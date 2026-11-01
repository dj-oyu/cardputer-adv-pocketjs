// Filesystem/provenance regression only. Synthetic proofs below test the loader
// contract; they do not assert native execution or rendered image correctness.
'use strict';
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),crypto=require('node:crypto'),assert=require('node:assert/strict'),{spawnSync}=require('node:child_process');
const {loadNative,png,compare}=require('./oracle.cjs'),{readNativeArtifacts}=require('./test_native_pixels.cjs');
const ROOT=path.resolve(__dirname,'../../..'),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const required=['tools/ayatsuri/asset/render_native.c','main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_model.h','main/ayatsuri/ayatsuri_transform.h','main/ayatsuri/ayatsuri_shade.h','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/ayatsuri_visibility.h','main/ui/kasane/ksn_procedural.c','main/ui/kasane/ksn_procedural.h'];
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'oracle-cli-test-'));let checks=0;
try{
 const dir=path.join(tmp,'synthetic-loader-contract');fs.mkdirSync(dir);const bin=Buffer.from('synthetic loader input, not native output'),inputHash=hash(bin);
 const metadata={width:240,height:135,pixelFormat:'RGB565_U16LE',camera:[0,0,400,0,0,0,1,120,67],light:[-.5,-.4,.8,1,1,1],rootPose:[0,0,0,0,0,0,1,1,1],rootPoseApplied:false};
 const outputs={'metadata.json':Buffer.from(JSON.stringify(metadata)),'pixels.u16le':Buffer.alloc(64800),'coverage.u16le':Buffer.alloc(64800),'visible-triangles.u16le':Buffer.alloc(64800)};
 for(const [name,b]of Object.entries(outputs))fs.writeFileSync(path.join(dir,name),b);
 const proof={passed:true,inputUnchanged:true,sourcesUnchanged:true,inputSha256:inputHash,requestedParameters:{camera:metadata.camera,light:metadata.light,rootPose:metadata.rootPose,rootPoseApplied:false},sourceSha256:Object.fromEntries(required.map(f=>[f,hash(fs.readFileSync(path.join(ROOT,f)))])),outputs:Object.fromEntries(Object.entries(outputs).map(([f,b])=>[f,hash(b)]))};
 const write=p=>fs.writeFileSync(path.join(dir,'render-report.json'),JSON.stringify(p));
 const load=[()=>loadNative(dir,inputHash),()=>readNativeArtifacts(dir,bin)];
 write(proof);for(const f of load){assert.equal(f().pixels.length,32400);checks++;}
 const reject=(change,code)=>{const p=structuredClone(proof);change(p);write(p);for(const f of load){assert.throws(f,e=>e.message===code);checks++;}};
 reject(p=>delete p.sourceSha256,'NATIVE_SOURCE_STALE');reject(p=>p.sourceSha256={},'NATIVE_SOURCE_STALE');
 reject(p=>delete p.sourceSha256[required[0]],'NATIVE_SOURCE_STALE');reject(p=>p.sourceSha256[required[0]]='0'.repeat(64),'NATIVE_SOURCE_STALE');
 reject(p=>p.sourcesUnchanged=false,'NATIVE_PROVENANCE');reject(p=>p.inputSha256='0'.repeat(64),'NATIVE_PROVENANCE');
 reject(p=>delete p.outputs,'NATIVE_OUTPUT_HASH');reject(p=>p.outputs['pixels.u16le']='0'.repeat(64),'NATIVE_OUTPUT_HASH');
 reject(p=>delete p.outputs['metadata.json'],'NATIVE_OUTPUT_HASH');
 reject(p=>delete p.requestedParameters,'NATIVE_REQUESTED_PARAMETERS');reject(p=>p.requestedParameters.light=[.6,-.2,.75,1,1,1],'NATIVE_PARAMETER_ECHO');
 // A rehashed metadata edit must still fail the independently saved request echo.
 const changedMeta={...metadata,light:[.6,-.2,.75,1,1,1]},changedBytes=Buffer.from(JSON.stringify(changedMeta)),changedProof=structuredClone(proof);
 changedProof.outputs['metadata.json']=hash(changedBytes);fs.writeFileSync(path.join(dir,'metadata.json'),changedBytes);write(changedProof);
 for(const f of load){assert.throws(f,/NATIVE_PARAMETER_ECHO/);checks++;}fs.writeFileSync(path.join(dir,'metadata.json'),outputs['metadata.json']);
 write(proof);const pixel=path.join(dir,'pixels.u16le');fs.writeFileSync(pixel,Buffer.alloc(64800,1));for(const f of load){assert.throws(f,/NATIVE_OUTPUT_HASH/);checks++;}fs.writeFileSync(pixel,outputs['pixels.u16le']);
 // Existing directory protection happens before any input read or output write.
 const cli=path.join(__dirname,'oracle.cjs'),out=path.join(tmp,'existing');fs.mkdirSync(out);const sentinels=['oracle.json','source.png','native.png','depth.png','diff.png'];
 for(const f of sentinels)fs.writeFileSync(path.join(out,f),'KEEP '+f);
 const before=Object.fromEntries(sentinels.map(f=>[f,hash(fs.readFileSync(path.join(out,f)))]));
 const command=target=>spawnSync(process.execPath,[cli,'missing-ir','missing-report','missing-native','missing-thresholds',target],{encoding:'utf8'});
 let r=command(out);assert.equal(r.status,1);assert.match(r.stderr,/EEXIST/);for(const f of sentinels)assert.equal(hash(fs.readFileSync(path.join(out,f))),before[f]);checks++;
 const fileOut=path.join(tmp,'existing-file');fs.writeFileSync(fileOut,'KEEP FILE');r=command(fileOut);assert.equal(r.status,1);assert.match(r.stderr,/EEXIST/);assert.equal(fs.readFileSync(fileOut,'utf8'),'KEEP FILE');checks++;
 const fresh=path.join(tmp,'nested','reserved');r=command(fresh);assert.equal(r.status,1);assert(fs.statSync(fresh).isDirectory());assert.deepEqual(fs.readdirSync(fresh),[]);r=command(fresh);assert.equal(r.status,1);assert.match(r.stderr,/EEXIST/);checks++;
 const image=path.join(tmp,'image.png');fs.writeFileSync(image,'KEEP IMAGE');assert.throws(()=>png(image,new Uint16Array(32400)),/EEXIST/);assert.equal(fs.readFileSync(image,'utf8'),'KEEP IMAGE');checks++;
 // An optional real artifact can exercise the shared loader without fabricating
 // source-hash currency or mutating an old/stale native proof.
 if(process.argv[2]&&!process.argv[2].startsWith('--')){const real=path.resolve(process.argv[2]),p=JSON.parse(fs.readFileSync(path.join(real,'render-report.json'))),a=loadNative(real,p.inputSha256),b=readNativeArtifacts(real,fs.readFileSync(process.argv[3]||p.input));assert.deepEqual(a.pixels,b.pixels);checks++;}
 // A fresh full study must satisfy both loaders before its actual native buffer
 // can establish that changing only the requested light fails independent gates.
 const lightIndex=process.argv.indexOf('--lighting-study');
 if(lightIndex>=0){assert(process.argv[lightIndex+1],'--lighting-study requires a fresh study directory');const base=path.resolve(process.argv[lightIndex+1]),d=path.join(base,'v65-light'),read=f=>JSON.parse(fs.readFileSync(f));
  const proof=read(path.join(d,'render-report.json')),bin=fs.readFileSync(path.join(base,'candidate65/candidate.aynm'));
  const native=loadNative(d,hash(bin)),shared=readNativeArtifacts(d,bin);assert.deepEqual(native,shared);checks+=2;
  const sourceIR=read(path.join(base,'candidate65/scene.ir.json')),compileReport=read(path.join(base,'candidate65/report.json')),thresholds=read(path.join(base,'thresholds.json'));
  const original=compare({sourceIR,compileReport,...native,thresholds}).report;assert.equal(original.passed,true);checks++;
  // Clone metadata so neither the loaded proof nor any artifact is changed.
  const metadata=structuredClone(native.metadata);metadata.light=[-.5,-.4,.8,1,1,1];
  assert.notDeepEqual(metadata.light,native.metadata.light);
  const r=compare({sourceIR,compileReport,...native,metadata,thresholds}).report;
  assert.equal(r.passed,false);assert.equal(r.gates.lighting,false);assert.equal(r.gates.color,false);for(const [gate,passed]of Object.entries(r.gates))if(!['lighting','color'].includes(gate))assert.equal(passed,true,gate);assert(r.metrics.colorChannels[1].mean>2);assert(r.metrics.lightingMaxError>1);checks++;
 }
 console.log(JSON.stringify({status:'PASS',checks,sharedLoader:true,missingStaleSourceProofRejected:true,outputExclusive:true,nativeExecutionClaim:false}));
}finally{fs.rmSync(tmp,{recursive:true,force:true});}
