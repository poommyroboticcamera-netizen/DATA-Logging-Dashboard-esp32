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
  function rotate(q, v) { return multiply(multiply(q, [0, ...v]), conjugate(q)).slice(1); }
  function blend(a, b, t) {
    const sign = a.reduce((s,v,i) => s + v*b[i], 0) < 0 ? -1 : 1;
    const q = a.map((v,i) => v*(1-t) + sign*b[i]*t);
    const norm = Math.hypot(...q);
    return q.map(v => v/norm);
  }
  function sample(data) {
    const number = key => data?.[key] === '' || data?.[key] == null ? NaN : Number(data[key]);
    if (String(data?.imu_valid) !== '1' || String(data?.tilt_valid) !== '1' || String(data?.roll_valid) !== '1') return null;
    const roll = number('roll_deg'), pitch = number('pitch_deg');
    const yawValid = String(data?.yaw_valid) === '1' && Number.isFinite(number('yaw_deg'));
    if (![roll,pitch].every(Number.isFinite)) return null;
    return { q: attitude(roll,pitch,yawValid ? number('yaw_deg') : 0), yawValid };
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
    const scene = canvas.parentElement;
    const vertices = [[1.4,-.8,.16],[1.4,.8,.16],[-1.4,.8,.16],[-1.4,-.8,.16],
      [1.4,-.8,-.16],[1.4,.8,-.16],[-1.4,.8,-.16],[-1.4,-.8,-.16]];
    const faces = [[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]];
    const colors = ['#2f70ff','#23bce7','#153d9b','#166984','#1c315c','#275285'];
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
      const project = v => { const [x,y,z] = cameraPoint(v); return [width/2+x*scale,height*.52-y*scale,z]; };
      const line = (a,b,color,lineWidth=1) => { ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.strokeStyle=color;ctx.lineWidth=lineWidth;ctx.stroke(); };
      for (let n=-4;n<=4;n++) {
        line(project([n*.5,-2,0]),project([n*.5,2,0]),'#1c2c42');
        line(project([-2,n*.5,0]),project([2,n*.5,0]),'#1c2c42');
      }
      ctx.globalAlpha = valid ? 1 : .35;
      const q = multiply(conjugate(reference), shown);
      const points = vertices.map(v => project(rotate(q,v)));
      faces.map((indices,i) => ({indices,i,depth:indices.reduce((s,j)=>s+points[j][2],0)/indices.length})).sort((a,b)=>a.depth-b.depth).forEach(({indices,i}) => {
        ctx.beginPath(); indices.forEach((index,j) => j ? ctx.lineTo(...points[index].slice(0,2)) : ctx.moveTo(...points[index].slice(0,2)));ctx.closePath();
        ctx.fillStyle=colors[i];ctx.fill();ctx.strokeStyle='#88cbff';ctx.lineWidth=.8;ctx.stroke();
      });
      line(points[0],points[1],'#71edff',4);
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
      if (now-lastFrame<1000/20) {raf=requestAnimationFrame(frame);return;}
      const dt=Math.min(100,now-lastFrame);lastFrame=now;
      shown = reduced.matches ? target.slice() : blend(shown,target,1-Math.exp(-dt/65));
      draw();
      const dot=Math.abs(shown.reduce((s,v,i)=>s+v*target[i],0));
      if (dot<.999999) raf=requestAnimationFrame(frame);
    }
    function wake() { if (!raf && !paused && !document.hidden && visible && valid) raf=requestAnimationFrame(frame); }
    function update(data, stale, boot) {
      latestInput = {data, stale, boot, at: performance.now()};
      if (paused) return; // No quaternion math, redraw or animation timers while OFF.
      const value = stale ? null : sample(data);
      const nextSource = `${boot}/${data?.yaw_reference ?? ''}/${value?.yawValid ?? false}`;
      clearTimeout(expiry);
      if (!value) {valid=false;note();draw();return;}
      if (!hasPose || nextSource!==source) {reference=identity();shown=value.q.slice();}
      source=nextSource;target=value.q;valid=true;full=value.yawValid;hasPose=true;receivedAt=performance.now();
      note();wake();
      expiry=setTimeout(()=>{if(performance.now()-receivedAt>=2400){valid=false;note();draw();}},2500);
    }
    reset.onclick=()=>{if(valid){reference=target.slice();shown=target.slice();draw();}};
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
  return {attitude,rotate,blend,multiply,conjugate,sample,cameraPoint,create};
})();
if (typeof module !== 'undefined') module.exports = ImuModel;
