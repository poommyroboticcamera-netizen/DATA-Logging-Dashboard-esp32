const assert = require('node:assert/strict');
const M = require('./imu-model.js');
const near = (a,b) => a.forEach((v,i) => assert.ok(Math.abs(v-b[i])<1e-6, `${a} != ${b}`));
near(M.rotate(M.attitude(90,0,0),[0,1,0]),[0,0,1]);
near(M.rotate(M.attitude(0,90,0),[1,0,0]),[0,0,-1]);
near(M.rotate(M.attitude(0,0,90),[1,0,0]),[0,1,0]);
const q=M.attitude(23,-41,179);
near(M.multiply(M.conjugate(q),q),[1,0,0,0]);
const mid=M.blend(M.attitude(0,0,179),M.attitude(0,0,-179),.5);
near(M.rotate(mid,[1,0,0]),[-1,0,0]); // Shortest path across yaw wrap.
const packet = {imu_valid:'1',tilt_valid:'1',roll_valid:'1',yaw_valid:'1',roll_deg:'10',pitch_deg:'20',yaw_deg:'30',yaw_reference:'1'};
assert.equal(M.sample(packet).yawValid,true);
assert.equal(M.sample({...packet,yaw_valid:'0',yaw_deg:''}).yawValid,false);
assert.equal(M.sample({...packet,roll_deg:''}),null);
assert.equal(M.sample({...packet,tilt_valid:'0'}),null);
assert.equal(M.sample({...packet,pitch_deg:'Infinity'}),null);
near(M.sample({...packet,yaw_valid:'0',yaw_deg:''},30).q,M.sample(packet).q);
// Uneven network arrivals are interpolated on a delayed display timeline.
const history=new M.PoseBuffer(350);
history.push(M.attitude(0,0,0),0);
history.push(M.attitude(0,0,20),200);
history.push(M.attitude(0,0,47),470);
near(M.rotate(history.at(685),[1,0,0]),M.rotate(M.attitude(0,0,33.5),[1,0,0]));
near(history.at(2000),M.attitude(0,0,47)); // Never extrapolate through a dropout.
assert.equal(history.pending(2000),false);
history.clear();assert.equal(history.at(2000),null);
history.push(M.attitude(0,0,179),0);history.push(M.attitude(0,0,-179),200);
near(M.rotate(history.at(450),[1,0,0]),[-1,0,0]);
for(let i=1;i<=100;i++)history.push(M.attitude(0,0,i),200+i);
assert.ok(history.samples.length<=32);

// Exercise browser lifecycle without a network or a board.
let time=0, paints=0, timer, frames=[], intersect, resize;
global.performance={now:()=>time};global.devicePixelRatio=1;
global.matchMedia=()=>({matches:false});
global.document={hidden:false,addEventListener(){}};
global.requestAnimationFrame=fn=>(frames.push(fn),frames.length);
global.cancelAnimationFrame=()=>{frames=[];};
global.setTimeout=fn=>(timer=fn,1);global.clearTimeout=()=>{};
global.ResizeObserver=class{constructor(fn){resize=fn;}observe(){}};
global.IntersectionObserver=class{constructor(fn){intersect=fn;}observe(){}};
const ctx=new Proxy({}, {get:(o,k)=>o[k]??((...args)=>{
  if(k==='clearRect') paints++;
  for(const v of args) if(typeof v==='number') assert.ok(Number.isFinite(v));
}),set:(o,k,v)=>(o[k]=v,true)});
const element=()=>({disabled:false,textContent:'',setAttribute(){}});
const canvas={parentElement:{classList:{add(){},toggle(){}}},clientWidth:450,clientHeight:300,getContext:()=>ctx};
const status=element(),reset=element(),restore=element(),pause=element();
const view=M.create(canvas,status,reset,restore,pause);
const pump=()=>{const pending=frames;frames=[];time+=60;pending.forEach(fn=>fn(time));};
view.update(packet,false,'boot1');pump();assert.equal(paints,0,'OFF by default: no canvas work');assert.equal(frames.length,0);
pause.onclick();pump();pump();assert.equal(reset.disabled,false);
const original=JSON.stringify(packet);reset.onclick();restore.onclick();assert.equal(JSON.stringify(packet),original);
view.update({...packet,yaw_valid:'0',yaw_deg:''},false,'boot1');pump();
assert.equal(reset.disabled,false,'Roll/Pitch reference remains available before yaw is ready');
pause.onclick();assert.equal(reset.disabled,true);const offPaints=paints;
view.update({...packet,yaw_deg:'50'},false,'boot1');resize();pump();assert.equal(paints,offPaints);assert.equal(frames.length,0);
pause.onclick();pump();assert.equal(reset.disabled,false);
view.update(packet,true,'boot1');assert.equal(reset.disabled,true);pump();assert.equal(frames.length,0);
view.update(packet,false,'boot2');pump();time+=2600;timer();assert.equal(reset.disabled,true);
intersect([{isIntersecting:false}]);const before=paints;
view.update(packet,false,'boot2');resize();pump();assert.equal(paints,before);
intersect([{isIntersecting:true}]);pump();assert.ok(paints>before);
canvas.clientWidth=320;resize();assert.equal(canvas.width,320);
console.log('PASS: model axes, reference, yaw wrap, invalid/partial data, pause, stale, visibility, resize; no packet mutations');

const front=M.cameraPoint([1,0,0]);assert.ok(Math.abs(front[0])<1e-9);assert.ok(front[1]>0,"yaw zero projects toward top of screen");
assert.ok(M.cameraPoint([0,0,1])[1]>0,"Z axis upward");
