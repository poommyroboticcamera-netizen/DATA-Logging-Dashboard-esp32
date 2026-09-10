/* Browser-only attitude preview. No requests, sensor commands or log mutations. */
const ImuModel = (() => {
  const identity = () => [1, 0, 0, 0];
  const conjugate = q => [q[0], -q[1], -q[2], -q[3]];
  function multiply(a, b) {
    const [w,x,y,z] = a, [v,i,j,k] = b;
    return [w*v-x*i-y*j-z*k, w*i+x*v+y*k-z*j, w*j-x*k+y*v+z*i, w*k+x*j-y*i+z*v];
  }
  function attitude(roll, pitch, yaw) {
    const r = roll * Math.PI / 360, p = pitch * Math.PI / 360, y = yaw * Math.PI / 360;
    return multiply(multiply([Math.cos(y),0,0,Math.sin(y)], [Math.cos(p),0,Math.sin(p),0]), [Math.cos(r),Math.sin(r),0,0]);
  }
  function rotate(q, v) {
    const [w,x,y,z]=q, [a,b,c]=v;
    const tx=2*(y*c-z*b),ty=2*(z*a-x*c),tz=2*(x*b-y*a);
    return [a+w*tx+y*tz-z*ty,b+w*ty+z*tx-x*tz,c+w*tz+x*ty-y*tx];
  }
  function blend(a, b, t) {
    const sign = a.reduce((s,v,i) => s + v*b[i], 0) < 0 ? -1 : 1;
    const q = a.map((v,i) => v*(1-t) + sign*b[i]*t);
    const norm = Math.hypot(...q);
    return q.map(v => v/norm);
  }
  // Play a short, bounded history instead of chasing each network packet.
  // Interpolation never predicts motion beyond the latest measured pose.
  class PoseBuffer {
    constructor(delay=350) { this.delay=delay; this.samples=[]; }
    clear() { this.samples=[]; }
    push(q,time) {
      const last=this.samples[this.samples.length-1];
      if(last && time<=last.time) return;
      this.samples.push({q:q.slice(),time});
      if(this.samples.length>32)this.samples.shift();
    }
    at(now) {
      const t=now-this.delay;
      while(this.samples.length>2 && this.samples[1].time<=t)this.samples.shift();
      const a=this.samples[0],b=this.samples[1];
      if(!a)return null;
      if(!b || t<=a.time)return a.q;
      return blend(a.q,b.q,Math.min(1,(t-a.time)/(b.time-a.time)));
    }
    pending(now) { return this.samples.length>0 && now-this.delay<this.samples[this.samples.length-1].time; }
  }
  function sample(data, fallbackYaw=0) {
    const number = key => data?.[key] === '' || data?.[key] == null ? NaN : Number(data[key]);
    if (String(data?.imu_valid) !== '1' || String(data?.tilt_valid) !== '1' || String(data?.roll_valid) !== '1') return null;
    const roll = number('roll_deg'), pitch = number('pitch_deg');
    const yawValid = String(data?.yaw_valid) === '1' && Number.isFinite(number('yaw_deg'));
    if (![roll,pitch].every(Number.isFinite)) return null;
    const yaw=yawValid ? number('yaw_deg') : fallbackYaw;
    return { q: attitude(roll,pitch,yaw), yawValid, yaw };
  }
  // Fixed elevated camera: +X (front / yaw zero) and +Z project upward.
  function cameraPoint([x,y,z]) {
    const elevation = Math.PI / 3;
    return [-y, Math.sin(elevation)*x + Math.cos(elevation)*z,
      -Math.cos(elevation)*x + Math.sin(elevation)*z];
  }
  function create(canvas, status, reset, restore, pause) {
    const ctx = canvas.getContext('2d');
    if (!ctx) { status.textContent = 'เบราว์เซอร์ไม่รองรับ Canvas'; return { update() {} }; }
    let target = identity(), shown = identity(), reference = identity();
    let hasPose = false, valid = false, full = false, paused = true, visible = true;
    let source = '', receivedAt = 0, raf = 0, lastFrame = 0, expiry;
    const reduced = matchMedia('(prefers-reduced-motion: reduce)');
    let latestInput = null;
    let lastYaw=0;
    const motion=new PoseBuffer();
    const scene = canvas.parentElement;
    // Lightweight mesh, built once. +X is the robot's front; the wheels are
    // fixed to the chassis because IMU data does not measure wheel travel.
    const mesh = [];
    function box(x,y,z,l,w,h,colors) {
      const v=[[x+l/2,y-w/2,z+h/2],[x+l/2,y+w/2,z+h/2],
        [x-l/2,y+w/2,z+h/2],[x-l/2,y-w/2,z+h/2],
        [x+l/2,y-w/2,z-h/2],[x+l/2,y+w/2,z-h/2],
        [x-l/2,y+w/2,z-h/2],[x-l/2,y-w/2,z-h/2]];
      [[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]].forEach((f,i)=>
        mesh.push({v:f.map(j=>v[j]),color:colors[i%colors.length]}));
    }
    const metal=['#e1e7ed','#596574','#aebbc8','#8694a4','#687788','#a5b2bf'];
    box(0,0,0,2.65,1.85,.48,metal);
    box(-.12,0,.32,1.5,1.25,.2,metal);
    box(-.48,0,.85,.77,.76,.94,metal); // Sensor tower.
    box(-.48,0,1.36,.86,.83,.15,['#b9c2cc','#536171','#7d8b9b']);
    box(-.083,0,.96,.026,.51,.2,['#172a3e']); // Front sensor window.
    box(-.062,0,.96,.022,.23,.065,['#55d3df']);
    box(.62,0,.29,.52,.65,.045,['#367d68','#24493e']); // Electronics deck.
    box(.65,0,.33,.17,.2,.045,['#283b43']);
    box(1.34,0,.06,.03,1.36,.055,['#58d6df']); // Front reference stripe.
    for(const y of [-.64,.64]) box(1.35,y,-.04,.05,.2,.15,['#b4e9e6']);
    for(const y of [-1.05,1.05]) box(0,y,-.23,3,.075,.075,metal);
    for(const x of [-1.47,1.47]) box(x,0,-.23,.075,2.16,.075,metal);
    for(const x of [-.91,.91]) for(const y of [-.99,.99]) {
      const n=12, radius=.38, half=.19;
      const ring=side=>Array.from({length:n},(_,i)=>{
        const a=i*2*Math.PI/n;return [x+radius*Math.cos(a),y+side*half,-.22+radius*Math.sin(a)];
      });
      const a=ring(-1),b=ring(1);
      mesh.push({v:a,color:'#262e37'},{v:b,color:'#303944'});
      for(let i=0;i<n;i++)mesh.push({v:[a[i],a[(i+1)%n],b[(i+1)%n],b[i]],color:i%2?'#171e26':'#414b56'});
      box(x,y+Math.sign(y)*.2,-.22,.17,.025,.17,['#a1b0bf']);
    }
    function note() {
      status.textContent = paused ? 'ปิดโมเดล 3D · ค่าเซนเซอร์และการบันทึกยังทำงาน' : !valid ? (hasPose ? 'พักภาพล่าสุด · รอค่า IMU / มุมเอียงที่ใช้ได้' : 'รอค่า IMU') : full ? 'มุมจาก IMU · ภาพประมาณจาก Roll / Pitch / Yaw' : 'แสดงเฉพาะ Roll / Pitch · ยังไม่มีค่า Yaw';
      // A visual reference is useful for Roll/Pitch as well; do not keep this
      // control disabled merely because startup yaw calibration is incomplete.
      reset.disabled = !valid || paused;
      restore.disabled = paused;
    }
    function draw() {
      if (paused || document.hidden || !visible) return;
      const width = canvas.clientWidth, height = canvas.clientHeight;
      if (!width || !height) return;
      const dpr = Math.min(devicePixelRatio || 1, 2);
      if (canvas.width !== Math.round(width*dpr) || canvas.height !== Math.round(height*dpr)) {
        canvas.width = Math.round(width*dpr); canvas.height = Math.round(height*dpr);
      }
      ctx.setTransform(dpr,0,0,dpr,0,0); ctx.clearRect(0,0,width,height);
      const scale = Math.min(width/6.4,height/5.1);
      const project = v => { const [x,y,z] = cameraPoint(v); return [width/2+x*scale,height*.59-y*scale,z]; };
      const line = (a,b,color,lineWidth=1) => { ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.strokeStyle=color;ctx.lineWidth=lineWidth;ctx.stroke(); };
      for (let n=-4;n<=4;n++) {
        line(project([n*.5,-2,0]),project([n*.5,2,0]),'#1c2c42');
        line(project([-2,n*.5,0]),project([2,n*.5,0]),'#1c2c42');
      }
      ctx.globalAlpha = valid ? 1 : .35;
      const q = multiply(conjugate(reference), shown);
      mesh.map(face=>{
        const points=face.v.map(v=>project(rotate(q,v)));
        return {points,color:face.color,depth:points.reduce((s,p)=>s+p[2],0)/points.length};
      }).sort((a,b)=>a.depth-b.depth).forEach(face=>{
        ctx.beginPath();face.points.forEach((p,i)=>i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]));ctx.closePath();
        ctx.fillStyle=face.color;ctx.fill();ctx.strokeStyle='#455366';ctx.lineWidth=.45;ctx.stroke();
      });
      const origin = project([0,0,0]);
      [[2,0,0],[0,1.8,0],[0,0,1.7]].forEach((v,i) => {
        const end=project(rotate(q,v)),color=['#ff7585','#53dec0','#a99aff'][i];
        line(origin,end,color,2);ctx.fillStyle=color;ctx.font='bold 13px monospace';ctx.fillText(['X','Y','Z'][i],end[0]+7,end[1]-7);
      });
      ctx.globalAlpha=1;
      if (!hasPose) {ctx.fillStyle='#a8b8cb';ctx.textAlign='center';ctx.font='13px sans-serif';ctx.fillText('รอข้อมูลการเอียงจากบอร์ด',width/2,height-18);ctx.textAlign='left';}
    }
    function frame(now) {
      raf=0;
      if (paused || document.hidden || !visible || !valid) return;
      // A small playback delay absorbs packet timing jitter. The final filter
      // softens velocity changes without repeatedly stopping at each packet.
      const dt=lastFrame ? Math.min(50,Math.max(0,now-lastFrame)) : 16;lastFrame=now;
      const playback=motion.at(now)||target;
      shown = reduced.matches ? target.slice() : blend(shown,playback,1-Math.exp(-dt/65));
      draw();
      const dot=Math.abs(shown.reduce((s,v,i)=>s+v*target[i],0));
      if (!reduced.matches && (motion.pending(now)||dot<.99999999)) raf=requestAnimationFrame(frame);
    }
    function wake() { if (!raf && !paused && !document.hidden && visible && valid) raf=requestAnimationFrame(frame); }
    function update(data, stale, boot) {
      latestInput = {data, stale, boot, at: performance.now()};
      if (paused) return; // No quaternion math, redraw or animation timers while OFF.
      const nextSource = `${boot}/${data?.yaw_reference ?? ''}`;
      const changedSource=nextSource!==source;
      const value = stale ? null : sample(data,changedSource ? 0 : lastYaw);
      clearTimeout(expiry);
      if (!value) {valid=false;note();draw();return;}
      if (!hasPose || changedSource) {reference=identity();shown=value.q.slice();lastFrame=0;motion.clear();}
      else if (!valid || !raf) lastFrame=0;
      if(!valid)motion.clear();
      lastYaw=value.yaw;
      motion.push(value.q,performance.now());
      source=nextSource;target=value.q;valid=true;full=value.yawValid;hasPose=true;receivedAt=performance.now();
      note();wake();
      expiry=setTimeout(()=>{if(performance.now()-receivedAt>=2400){valid=false;note();draw();}},2500);
    }
    reset.onclick=()=>{if(valid){reference=shown.slice();draw();}};
    restore.onclick=()=>{reference=identity();draw();};
    pause.onclick=()=>{
      paused=!paused;
      pause.textContent=paused?'เปิดโมเดล 3D':'ปิดโมเดล 3D';
      pause.setAttribute('aria-pressed',String(!paused));
      scene.classList.toggle('imu-scene-off',paused);
      if(paused) {
        clearTimeout(expiry);
        if(raf) cancelAnimationFrame(raf);
        raf=0;note();
        motion.clear();lastFrame=0;
      } else {
        if(latestInput) update(latestInput.data,latestInput.stale || performance.now()-latestInput.at>2500,latestInput.boot);
        note();draw();wake();
      }
    };
    scene.classList.add('imu-scene-off');
    pause.textContent='เปิดโมเดล 3D';pause.setAttribute('aria-pressed','false');
    new ResizeObserver(()=>{draw();wake();}).observe(canvas);
    new IntersectionObserver(entries=>{visible=entries[0].isIntersecting;if(visible){draw();wake();}}).observe(canvas);
    document.addEventListener('visibilitychange',()=>{if(!document.hidden){draw();wake();}});
    note();draw();
    return {update};
  }
  return {attitude,rotate,blend,multiply,conjugate,sample,cameraPoint,PoseBuffer,create};
})();
if (typeof module !== 'undefined') module.exports = ImuModel;
