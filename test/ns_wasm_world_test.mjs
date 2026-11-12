import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const ns=process.argv[2];assert(ns);
const root=fs.mkdtempSync(path.join(os.tmpdir(),'ns-ui-world-'));
try{
 fs.writeFileSync(path.join(root,'ns.mod'),'schema="ns.mod/v2"\nname="world-ui"\nversion="0.1.0"\ntype="app"\ntarget="wasm"\nsource="src"\nentry="main.ns"\n');
 fs.mkdirSync(path.join(root,'src'));
 fs.writeFileSync(path.join(root,'src/main.ns'),`use view
use ui
let v = view_create("World UI", 400, 300)
let r = ui_renderer_create(v)
let nested = true
fn nested_result() bool { return nested }
fn main() {
    ui_begin_frame(r)
    let vp = [1.0,0.0,0.0,0.0, 0.0,1.0,0.0,0.0, 0.0,0.0,1.0,0.0, 0.0,0.0,0.0,1.0]
    let model = [1.0,0.0,0.0,0.0, 0.0,1.0,0.0,0.0, 0.0,0.0,1.0,0.0, 0.25,0.0,0.0,1.0]
    if ui_begin_world(r, vp, model) {
        nested = ui_begin_world(r, vp, model)
        ui_fill_rect(r, 0.0, 0.0, 10.0, 10.0, 0xffffffffu, 0.0)
        ui_end_world(r)
    }
    model[12] = 0.75
    if ui_begin_world(r, vp, model) {
        ui_fill_rect(r, 0.0, 0.0, 10.0, 10.0, 0xffffffffu, 0.0)
        ui_end_world(r)
    }
    ui_fill_rect(r, 0.0, 0.0, 10.0, 10.0, 0xffffffffu, 0.0)
}
`);
 const result=spawnSync(ns,['build',root],{encoding:'utf8'});
 assert.equal(result.status,0,result.stdout+result.stderr);
 const source=fs.readFileSync(new URL('../lib/ns-wasm.js',import.meta.url),'utf8');
 const {NSBrowserRuntime}=await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
 globalThis.window={devicePixelRatio:1,addEventListener(){}};
 const canvas={width:400,height:300,clientWidth:400,clientHeight:300,style:{},addEventListener(){},setAttribute(){},getBoundingClientRect(){return{left:0,top:0,width:400,height:300};}};
 const runtime=new NSBrowserRuntime(canvas);
 const module=await WebAssembly.compile(fs.readFileSync(path.join(root,'bin/world-ui.wasm')));
 const instance=await WebAssembly.instantiate(module,runtime.importsFor(module));
 runtime.instance=instance;runtime.memory=instance.exports.memory;
 instance.exports.__ns_init();instance.exports.main();
 const r=[...runtime.uiRenderers.values()][0];
 assert.equal(instance.exports.nested_result(),0);
 assert.equal(r.commands.length,3);
 assert.equal(r.commands[0].world.model[12],.25);
 assert.equal(r.commands[1].world.model[12],.75);
 assert.notEqual(r.commands[0].world,r.commands[1].world);
 assert.equal(r.commands[2].world,undefined);
 assert.equal(r.world,null);assert.equal(r.clips.length,1);
 runtime.ui('ui_begin_frame',[[...runtime.uiRenderers.keys()][0]]);
 assert.equal(r.worldCount,0);assert.equal(r.commands.length,0);
 console.log('PASS: Wasm world API, copied matrices, non-nesting, distinct same-frame scopes and screen restoration.');
}finally{fs.rmSync(root,{recursive:true,force:true});}
