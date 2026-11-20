// Host-only probe: never flash or treat placeholder materials as the model.
'use strict';
const fs=require('fs'),{compileProvider}=require('../../ayatsuri/model/pack_model.cjs');
if(process.argv.length!==4)throw Error('expected probe manifest and new output');
const probe=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
if(probe.probe_kind!=='hone-provider-cold-only-v1'||probe.runtime_ready!==false)throw Error('diagnostic probe only');
fs.writeFileSync(process.argv[3],compileProvider(probe.payload),{flag:'wx'});
console.log('PASS diagnostic provider-only cold blob; placeholder materials; NOT runtime admission');
