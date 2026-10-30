// Keep the native byte initializer reproducible from the canonical AYNM file.
'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const root=path.resolve(__dirname,'../../..'),folder=path.join(root,'apps/cloud/assets');
function initializer(data){let out='// Generated from cloud.aynm; do not edit.\n';
  for(let i=0;i<data.length;i+=16)out+='  '+[...data.subarray(i,i+16)].map(x=>'0x'+x.toString(16).padStart(2,'0')).join(',')+',\n';
  return out;}
function check(write=false){const data=fs.readFileSync(path.join(folder,'cloud.aynm')),
  target=path.join(folder,'cloud_model_bytes.inc'),expected=initializer(data);
  if(write)fs.writeFileSync(target,expected);else if(fs.readFileSync(target,'utf8').replace(/\r\n/g,'\n')!==expected)throw Error('stale builtin cloud initializer');
  return {bytes:data.length,sha256:crypto.createHash('sha256').update(data).digest('hex')};}
module.exports={initializer,check};
if(require.main===module)console.log('AYNM_BUILTIN_PASS',JSON.stringify(check(process.argv.includes('--write'))));
