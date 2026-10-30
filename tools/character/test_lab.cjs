// Exercise the installed entry and chunk loader, including model replacement.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../..'),chunks={utility:'character.js',models:'character_models.js',lab:'character_lab_scene.js'};
const TransformReference=require('./transform_reference.cjs');
let active=false,commits=0,replaces=0,patches=0,textChanges=0,background=false;
const loaded=new Set();
const tx={background(color){assert.equal(color,0xe4ece0ff);background=true;},
  image(d){assert.ok(background,'replace starts with a background');assert.equal(d.resource,17);assert.deepEqual(Array.from(d.bounds),[0,0,240,135]);},
  text(d){assert.ok(typeof d.text==='string');const capacity=d.capacity||Buffer.byteLength(d.text);return {
    setText(transaction,text){assert.equal(transaction,tx);assert.equal(typeof text,'string');
      assert.ok(Buffer.byteLength(text)<=capacity,'patch stays in fixed text capacity');++textChanges;}};}};
const context=vm.createContext({console:{log(){}},pocket:{
  app:{load(name){assert.ok(chunks[name]);assert.ok(!loaded.has(name));loaded.add(name);
    vm.runInContext(fs.readFileSync(path.join(root,'apps/character',chunks[name]),'utf8'),context,{filename:chunks[name]});}},
  shapes:{ellipsoid(...args){assert.ok(active,'draw belongs to an app-owned candidate');
    assert.equal(args.length,6);
    [8,9,9,8,5,9].forEach((n,i)=>{assert.equal(args[i].length,n);assert.ok(Array.from(args[i]).every(Number.isFinite));});}},
  kasane:{procedural:{resource(d){assert.equal(d.maxSegments,1024);return 17;},
    beginFrame(){assert.ok(!active);active=true;},commit(){assert.ok(active);active=false;++commits;}},
    replace(callback){background=false;callback(tx);assert.ok(background);++replaces;},patch(callback){callback(tx);++patches;}}
}});
context.pocket.hone=context.pocket.shapes;
context.pocket.ayatsuri=TransformReference;
vm.runInContext(fs.readFileSync(path.join(root,'apps/character/character_lab.js'),'utf8'),context,{filename:'character_lab.js'});
assert.deepEqual(Array.from(loaded),['utility','models','lab']);
const frame=context.frame;
for(let model=0;model<3;model++){
  for(let mode=0;mode<6;mode++){
    frame(0);frame(0x20);frame(0);
    for(let i=0;i<10;i++)frame(0);
  }
  frame(0x10);frame(0);frame(0x40);frame(0);frame(0x4000);frame(0);
}
assert.equal(active,false);assert.ok(commits>200);assert.ok(replaces>=4);
assert.ok(patches>100);assert.ok(textChanges<40,'labels update only when their meaning changes');
console.log('CHARACTER_LAB_ENTRY_PASS commits='+commits+' replacements='+replaces+' textChanges='+textChanges);
