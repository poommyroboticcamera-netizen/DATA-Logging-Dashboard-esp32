/* Exclusive CAN workspace. No raw-frame history or CAN CSV is buffered in browser RAM. */
const CanDashboardCore = (() => {
  const hex = n => '0x' + Number(n).toString(16).toUpperCase();
  const states = {STOPPED:'ปิดไฟล์แล้ว',OPENING:'กำลังสร้างไฟล์',RECORDING:'กำลังบันทึกลง SD',CLOSING:'กำลังเขียนข้อมูลค้างและปิดไฟล์',MOUNT_FAILED:'เปิด SD ไม่สำเร็จ · ตรวจการ์ดและสาย',OPEN_FAILED:'สร้างไฟล์ CSV ไม่สำเร็จ',WRITE_FAILED:'เขียน SD ไม่สำเร็จ · ข้อมูลบางส่วนอาจสูญหาย',STOPPED_WITH_ERRORS:'ปิดไฟล์แล้ว แต่พบข้อผิดพลาดการเขียน'};
  const driverStates = {DISABLED:'CAN ปิดอยู่',STARTING:'กำลังเปิด CAN',LISTENING:'CAN กำลังรับข้อมูล',STOPPING:'กำลังปิด CAN',START_FAILED:'เปิด CAN ไม่สำเร็จ'};
  function packet(p) {
    if(!p || p.mode!=='LISTEN_ONLY' || !Array.isArray(p.ids) || !Array.isArray(p.candidates) || p.ids.length>128 || p.candidates.length>12)throw Error('รูปแบบข้อมูล CAN ไม่ตรงกับเฟิร์มแวร์');
    return p;
  }
  function capture(label, seconds, baseline=false) {
    const n=Number(seconds);
    if(!Number.isInteger(n) || n<1 || n>3600)throw Error('กำหนดเวลา 1–3600 วินาที');
    if(baseline)return `BASELINE ${n}`;
    const name=String(label).trim().toUpperCase();
    if(!/^[A-Z0-9_-]{1,23}$/.test(name))throw Error('ชื่อการทดลองใช้ A–Z, 0–9, _ หรือ - ไม่เกิน 23 ตัว');
    return `EXPERIMENT ${name} ${n}`;
  }
  return {hex,states,driverStates,packet,capture};
})();
if(typeof module!=='undefined')module.exports=CanDashboardCore;
if(typeof document!=='undefined')(() => {
  'use strict';
  const $=id=>document.getElementById(id), C=CanDashboardCore;
  let mode='dashboard', changing=false, busy=false, closing=false, lastSuccess=0, lastPacket=null;
  window.DashboardModes={current:()=>mode,interval:()=>mode==='dashboard'?250:1000};
  function note(text,error=false) { $('mode-status').textContent=text;$('mode-status').classList.toggle('mode-error',error); }
  function showMode(next) {
    mode=next;
    document.querySelector('.workspace').hidden=mode!=='dashboard';
    $('mode-can').hidden=mode!=='can';
    document.querySelectorAll('[data-mode]').forEach(b=>{const selected=b.dataset.mode===mode;b.classList.toggle('selected',selected);b.setAttribute('aria-selected',String(selected));});
    document.querySelectorAll('.sidebar nav a').forEach(a=>{const selected=mode==='can'?a.id==='nav-can':a.getAttribute('href')==='#overview';a.classList.toggle('active',selected);if(selected)a.setAttribute('aria-current','location');else a.removeAttribute('aria-current');});
  }
  async function post(path, values) {
    const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Dashboard-Request':'1'},body:new URLSearchParams(values),signal:AbortSignal.timeout(5000)});
    if(!response.ok)throw Error(await response.text());return response.json();
  }
  async function select(next) {
    if(changing || !['dashboard','can'].includes(next))return;
    if(next===mode)return;
    changing=true;
    document.querySelectorAll('[data-mode]').forEach(b=>b.disabled=true);
    try {
      await post('/api/can/mode',{mode:next});
      const previous=mode;closing=previous==='can' && next!=='can';showMode(next);
      if(mode==='can' && $('imu-model-pause').getAttribute('aria-pressed')==='true')$('imu-model-pause').click();
      note(mode==='can'?'เข้า CAN mode แล้ว · ฟังก์ชัน Dashboard ถูกพัก · กดเปิด CAN เมื่อตรวจสายและ Bitrate แล้ว':closing?'กำลังปิด CAN และไฟล์ CSV ก่อนกลับ Dashboard':'Dashboard กลับมาทำงานตามปกติ');
      if(mode==='can')pollCan();
    } catch(error) { note('เปลี่ยนโหมดไม่สำเร็จ: '+error.message,true); }
    finally {changing=false;document.querySelectorAll('[data-mode]').forEach(b=>b.disabled=false);}
  }
  async function send(command) {
    try {await post('/api/can/command',{command});note('ส่งคำสั่งแล้ว · รอผลจากบอร์ด');await pollCan();}
    catch(error){note(error.message,true);}
  }
  function cell(row,text) {const td=document.createElement('td');td.textContent=text;row.append(td);return td;}
  function renderCan(p) {
    if(p.demo){closing=false;note('ตัวอย่างหน้าเว็บเท่านั้น · ไม่มีเฟรม CAN หรือไฟล์ SD จริง');}
    $('can-log-state').textContent=C.states[p.log_state]||p.log_state;
    $('can-log-state').className='pill '+(p.log_state==='RECORDING'?'on':'');
    const requested=!!p.requested_enabled, listening=!!p.enabled;
    $('can-enable').textContent=requested?'ปิด CAN':'เปิด CAN';
    $('can-enable').setAttribute('aria-pressed',String(requested));
    $('can-enable').className=requested?'secondary':'primary';
    $('can-enable').disabled=['STARTING','STOPPING'].includes(p.driver_state);
    $('can-driver-state').textContent=C.driverStates[p.driver_state]||p.driver_state;
    $('can-driver-state').className='pill '+(listening?'on':'');
    if(document.activeElement!==$('can-bitrate'))$('can-bitrate').value=String(p.bitrate);
    const changingDriver=requested || listening || ['STARTING','STOPPING'].includes(p.driver_state);
    $('can-bitrate').disabled=changingDriver;$('can-bitrate-apply').disabled=changingDriver;
    $('can-log-start').disabled=!listening;
    $('can-file').textContent=p.file||'ยังไม่มีไฟล์';
    $('can-frame-count').textContent=Number(p.frames).toLocaleString();
    $('can-id-count').textContent=`${p.ids.length} / ${p.capacity}`;
    $('can-drops').textContent=`RX ${p.driver_missed+p.driver_overruns} · วิเคราะห์ ${p.analysis_drops} · CSV ${p.log_drops}`;
    $('can-memory').textContent=p.demo?'ไม่วัด RAM ในหน้า demo':`ว่าง ${Math.round(p.free_heap/1024)} KiB · ต่ำสุด ${Math.round(p.min_heap/1024)} KiB`;
    $('can-wiring').textContent=`${p.bitrate/1000} kbit/s · TX GPIO${p.tx_gpio} / RX GPIO${p.rx_gpio} · ตรวจสายก่อนต่อรถ`;
    $('can-rows').textContent=`รับเข้า buffer/เขียนแล้ว ${Number(p.rows).toLocaleString()} แถว · write errors ${p.write_errors} · database-full frames ${p.db_full}`;
    const tbody=$('can-ids');tbody.replaceChildren();
    [...p.ids].sort((a,b)=>b.count-a.count).forEach(item=>{
      const row=document.createElement('tr');cell(row,C.hex(item.id));cell(row,(item.extended?'EXT':'STD')+(item.rtr?' RTR':''));
      cell(row,item.dlc);cell(row,Number(item.count).toLocaleString());cell(row,Number(item.hz).toFixed(1));
      cell(row,(item.data||[]).map(v=>Number(v).toString(16).padStart(2,'0').toUpperCase()).join(' ')||'—');
      const td=cell(row,'');const button=document.createElement('button');button.className='secondary';button.textContent='วิเคราะห์ ID';
      button.onclick=()=>send(`ID ${Number(item.id).toString(16)} ${item.extended?'EXT':'STD'}`);td.append(button);tbody.append(row);
    });
    $('can-empty').hidden=p.ids.length>0;
    $('can-phase').textContent=p.phase===1?`Baseline · เหลือประมาณ ${p.remaining_s} s`:p.phase===2?`Experiment · เหลือประมาณ ${p.remaining_s} s`:p.action_ready?'เปรียบเทียบครบแล้ว':p.baseline_ready?'Baseline พร้อม · เริ่มการทดลองได้':'ยังไม่มี Baseline';
    $('can-baseline').disabled=!listening || !!p.phase;$('can-experiment').disabled=!listening || !!p.phase || !p.baseline_ready;
    $('can-candidates').replaceChildren();
    p.candidates.forEach((c,i)=>{
      const li=document.createElement('li');li.textContent=`Candidate ${i+1} · ${C.hex(c.id)} ${c.extended?'EXT':'STD'} · start ${c.start}, ${c.width} bit · ${c.motorola?'Motorola':'Intel'} · score ${Number(c.score).toFixed(3)}`;$('can-candidates').append(li);
    });
    if(!p.candidates.length){const li=document.createElement('li');li.textContent='รอการทดลองครบสองช่วง หรือยังไม่มี candidate ที่ได้คะแนนถึงเกณฑ์';$('can-candidates').append(li);}
    if(p.write_errors || /FAILED|ERRORS/.test(p.log_state))note(C.states[p.log_state]||'มีข้อผิดพลาด SD · ตรวจข้อมูลตกหล่นก่อนใช้ผล',true);
    else if(closing && p.log_state==='STOPPED'){closing=false;note(`ปิดไฟล์ CAN แล้ว: ${p.file || 'ไม่มีไฟล์'} · ถอด SD ได้เมื่อไม่มีงานบันทึกเซนเซอร์`);}
    else if(mode==='can' && p.log_state==='RECORDING')note(`CAN รับแบบ Listen-only · กำลังเก็บ CSV ${p.file}`);
    else if(mode==='can' && listening)note(`CAN เปิดอยู่ที่ ${p.bitrate/1000} kbit/s · ฟังก์ชัน Dashboard ยังถูกพัก`);
    if(closing && !p.log && ['STOPPED_WITH_ERRORS','MOUNT_FAILED','OPEN_FAILED','WRITE_FAILED'].includes(p.log_state))closing=false;
  }
  async function pollCan() {
    if(busy || (mode!=='can'&&!closing) || document.hidden)return;
    busy=true;
    try {
      const r=await fetch('/api/can',{cache:'no-store',signal:AbortSignal.timeout(2500)});if(!r.ok)throw Error(await r.text());
      lastPacket=C.packet(await r.json());lastSuccess=Date.now();renderCan(lastPacket);
      if(lastPacket.report_revision!==lastReportRevision){
        const report=await fetch('/api/can/report',{cache:'no-store',signal:AbortSignal.timeout(2500)});
        if(report.ok){$('can-report').textContent=await report.text();lastReportRevision=lastPacket.report_revision;}
      }
    } catch(error){note('ยังยืนยันสถานะ CAN ไม่ได้: '+error.message,true);$('can-log-state').textContent='ขาดการเชื่อมต่อ · สถานะล่าสุดอาจเก่า';}
    finally{busy=false;}
  }
  let lastReportRevision=-1;
  document.querySelectorAll('[data-mode]').forEach(b=>b.addEventListener('click',()=>select(b.dataset.mode)));
  document.querySelectorAll('.sidebar a[href^="#"]').forEach(a=>a.addEventListener('click',async event=>{
    if(a.id==='nav-can'){event.preventDefault();await select('can');return;}
    if(mode!=='dashboard'){event.preventDefault();await select('dashboard');document.querySelector(a.getAttribute('href'))?.scrollIntoView();}
  }));
  $('can-baseline').onclick=()=>{try{send(C.capture('',$('can-seconds').value,true));}catch(e){note(e.message,true);}};
  $('can-experiment').onclick=()=>{try{send(C.capture($('can-label').value,$('can-seconds').value));}catch(e){note(e.message,true);}};
  $('can-log-start').onclick=()=>send('LOG START');$('can-log-stop').onclick=()=>send('LOG STOP');
  $('can-enable').onclick=async()=>{
    try{const enable=!(lastPacket?.requested_enabled||lastPacket?.enabled);await post('/api/can/control',{enabled:enable?'1':'0'});note(enable?'กำลังเปิด CAN แบบ Listen-only':'กำลังปิด CAN และไฟล์ CSV');await pollCan();}
    catch(error){note(error.message,true);}
  };
  $('can-bitrate-apply').onclick=async()=>{
    try{await post('/api/can/bitrate',{bitrate:$('can-bitrate').value});note('บันทึก Bitrate แล้ว · กดเปิด CAN เพื่อเริ่มรับข้อมูล');await pollCan();}
    catch(error){note(error.message,true);}
  };
  $('can-summary').onclick=()=>send('SUMMARY');
  $('can-reset').onclick=()=>send('RESET');
  async function next(){await pollCan();setTimeout(next,1000);}
  async function synchronizeMode() {
    try {
      const response=await fetch('/api/can',{cache:'no-store',signal:AbortSignal.timeout(2500)});
      if(response.ok) {
        lastPacket=C.packet(await response.json());
        if(lastPacket.active) {
          showMode('can');
          if($('imu-model-pause').getAttribute('aria-pressed')==='true')$('imu-model-pause').click();
          renderCan(lastPacket);
          note('กู้สถานะ CAN จากบอร์ดแล้ว · ฟังก์ชัน Dashboard ยังถูกพัก');
        }
      }
    } catch {}
    next();
  }
  synchronizeMode();
})();
