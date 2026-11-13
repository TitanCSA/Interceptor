const assert=require('node:assert/strict');
const {Interceptor}=require('../index.cjs');
async function run(){const io=new Interceptor();try{
 if(process.argv[2]==='fixture'){
  const native=require(process.env.INTERCEPTOR_SDK_NODE_PATH);const kinds=["keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"];
  assert.equal(io.initialize().ready,true);assert.equal(io.info().input_mask,3);assert.equal(io.info().controller_mask,15);
  assert.equal(io.devices().length,kinds.length);let b=await io.receive(0);assert.deepEqual(b.map(x=>x.kind),kinds);
  for(const e of b){assert(e.sequence>0x7fffffffffffffffn);if(e.kind==='keyboard')e.state.scan_code=48;if(e.kind==='mouse')e.state.y=-7;}
  assert.equal(io.send(b),kinds.length);assert.equal(io.send(b),0);let next=await io.receive(0);assert.throws(()=>io.send(b),/Stale/);next[0].blocked=true;assert.equal(io.send(next),kinds.length-1);
  b=await io.receive(0);native._fixtureMode(2);assert.throws(()=>io.send(b),e=>e.code===170 && e.accepted===1);assert.equal(io.send(b),kinds.length-1);native._fixtureMode(0);io.discard(await io.receive(0));
  for(const kind of ['keyboard','mouse','hid','xinput','xinputhid','gip'].filter(k=>!kinds.includes(k)))assert.throws(()=>io.inject({kind}),e=>e.code===50);
  native._fixtureMode(1);const pending=io.receive();await new Promise(r=>setTimeout(r,30));io.cancel();assert.equal((await pending).length,0);native._fixtureMode(0);
 }
}finally{io.close();}console.log('Product Node SDK contract passed');}
run().catch(e=>{console.error(e);process.exitCode=1});
