// Compare the shared C kernel to the pre-extraction JS transform semantics.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const Reference=require('./transform_reference.cjs');
const input=path.resolve(process.argv[2]||'.cache/ayatsuri-transform/vectors.json');
const vectors=JSON.parse(fs.readFileSync(input,'utf8'));
assert.ok(vectors.length>=400,'deterministic root/parent and boundary cases');
const bits=new DataView(new ArrayBuffer(4));
function ordered(value){
  bits.setFloat32(0,value);
  const word=bits.getUint32(0);
  return word>>>31?(~word)>>>0:(word|0x80000000)>>>0;
}
let compared=0,exact=0,maxUlp=0,failures=0,maxAbsolute=0;
for(let index=0;index<vectors.length;++index){
  const row=vectors[index],pose=Float32Array.from(row.pose),parent=row.parent&&Float32Array.from(row.parent),
    expected=new Float32Array(12).fill(13),prior=expected.slice();
  let status='OK';
  try{Reference.compose(pose,parent,expected);}catch(error){
    status=/overflow/.test(error.message)?'RANGE':'INVALID';
    assert.deepEqual(expected,prior,'reference failure must not publish partial output');
  }
  assert.equal(row.status,status,'status at vector '+index);
  if(status!=='OK'){assert.equal(row.output,null);++failures;continue;}
  assert.equal(row.output.length,12);
  const actual=Float32Array.from(row.output);
  for(let i=0;i<12;++i){
    ++compared;
    if(Object.is(actual[i],expected[i]))++exact;
    if(actual[i]===0&&expected[i]===0)
      assert.ok(Object.is(actual[i],expected[i]),'signed zero at '+index+':'+i);
    const ulp=Math.abs(ordered(actual[i])-ordered(expected[i]));
    maxUlp=Math.max(maxUlp,ulp);maxAbsolute=Math.max(maxAbsolute,Math.abs(actual[i]-expected[i]));
    // Different host libm implementations may straddle one binary32 tie.
    assert.ok(ulp<=1,'float32 equivalence at '+index+':'+i+' C='+actual[i]+' JS='+expected[i]+' ULP='+ulp);
  }
}
console.log('AYATSURI_TRANSFORM_ORACLE_PASS vectors='+vectors.length+' components='+compared+
  ' exact='+exact+' maxULP='+maxUlp+' maxAbsolute='+maxAbsolute+' rejected='+failures);
