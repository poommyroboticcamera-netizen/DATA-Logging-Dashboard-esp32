# Dashboard reference

เว็บฝังใน firmware ผ่าน `scripts/embed_dashboard.py` ไม่ต้องมี SD หรือเว็บเซิร์ฟเวอร์บนคอมพิวเตอร์ Build/Upload ครั้งเดียวอัปเดต firmware และ UI

## Files

| File | Responsibility |
|---|---|
| index.html / style.css | หน้าเว็บและ responsive theme |
| app.js | HTTP polling, controls และ IndexedDB |
| core.js | CSV parsing, settings และ recording sessions |
| imu-model.js | Canvas 3D และ quaternion display math บน browser |

## HTTP interface

GET `/api/state`: snapshot ปัจจุบัน, CSV header/row, boot ID, recording state, device mask และ settings

GET `/api/health`: reset reason, uptime, heap, task/control diagnostics, ระดับขาปุ่ม และสถานะ SD

POST: `/api/settings`, `/api/wifi`, `/api/device`, `/api/encoder`, `/api/shunt`, `/api/ga`, `/api/ga-frequency`, `/api/led`

POST ใช้ form fields และ header `X-Dashboard-Request: 1`; device controls ตรวจ boot/revision เพื่อป้องกันคำสั่งเก่าทับสถานะ Header นี้ไม่ใช่ authentication

`/api/wifi` บันทึก SSID/รหัสผ่านใน NVS แล้วรีสตาร์ต บอร์ดเปิด AP ตลอดเวลาโดยใช้ชื่อ `ESP` ตามด้วย IP ที่เราเตอร์แจก เช่น `ESP10.185.167.203`; ถ้ายังไม่มี IP ใช้ `ESP192.168.4.1` รหัส `88888888` และเข้า `192.168.4.1` เพื่อดูหน้าเว็บและแก้ค่าได้

## Recording behavior

- `recording: false` แสดงค่าปัจจุบันได้ แต่ไม่เพิ่มแถวบันทึก
- ค่าเริ่มต้นรอบบันทึก 1000 ms หากเคยตั้งค่าไว้ บอร์ดจะใช้ค่าที่บันทึกใน NVS
- Encoder อ่านสัญญาณ Z ที่ GPIO33 ขอบขาขึ้น (ไม่ใช้ A/B และไม่คูณ ×4) ค่าเริ่มต้น 1 พัลส์/รอบ Encoder, ล้อ 63 mm, ลูกกลิ้ง 43 mm → อัตราทด 63/43 ≈ 1.465116; การตั้งค่าเก่าของ A/B จะย้ายค่า P/R เป็น 1 โดยคงขนาดล้อและอัตราทดที่ปรับเอง ส่วนค่าเดิม 360/100/1 จะย้ายเป็นค่าเริ่มต้นใหม่ ตรวจจำนวนพัลส์ Z จริงจากคู่มือ Encoder ก่อนใช้
- เปิดเครื่อง GA ติดทั้งหมดระหว่างเริ่มงานบันทึก แล้วดับเมื่อพร้อมและยังไม่บันทึก; กด START ติดทั้งหมดจน SD เขียนแถวแรกสำเร็จ จากนั้น GA3→GA0 วิ่งตามรอบบันทึก; กด STOP ติดทั้งหมดระหว่างระบายคิวและปิด SD แล้วดับ; หาก SD ผิดพลาดหรือหลังรีเซตจะติดทั้งหมดเมื่อ MCP สั่งงานได้ (หลังรีเซตค้างจนกด START, SD ผิดพลาดค้างจนเขียนสำเร็จ)
- SD mount และเขียนเฉพาะตอนบันทึก หลัง STOP จะเขียนแถวที่ค้างให้ครบแล้ว unmount; สถานะผิดพลาดดูที่ `sd_status` และ `sd_dropped`
- เปลี่ยน recording session, boot, schema หรือ interval จะแยกรอบข้อมูล
- ขาดการเชื่อมต่อเกิน 6 s ปิดรอบที่รับมาแล้วเป็น connection_lost
- IndexedDB แยกตาม origin/browser; เปลี่ยน IP จะเห็นข้อมูลคนละชุด
- ใช้หนึ่งแท็บบันทึกต่อ origin ยังไม่มีการประสานการเขียนหลายแท็บ
- จำกัด 86,400 แถวต่อรอบแล้วแบ่งรอบอัตโนมัติ รายการเก่ายังอยู่จนถูกจัดการโดยผู้ใช้/browser
- Polling มีคำขอเดียวค้างอยู่และเว้น 100–250 ms หลังจบรอบ อัตราจริงอาจช้ากว่ารอบบันทึก
- ไม่มีการเติมข้อมูลที่พลาดระหว่าง Wi-Fi ขาดหรือ browser ถูกพัก

## IMU model

โมเดลสี่เหลี่ยมเริ่มต้น OFF เมื่อเปิดจำกัด 20 FPS หยุดวาดเมื่อ OFF, พ้น viewport หรือซ่อนแท็บ มุมอ้างอิงปรับเฉพาะภาพ ไม่เปลี่ยน CSV เมื่อ Roll/Pitch/Yaw = 0 ด้านหน้า +X ชี้ขึ้นจอ

## Tests

```sh
node dashboard/core.test.cjs
node dashboard/imu-model.test.cjs
node --check dashboard/app.js
node scripts/test_ga_demo.cjs
```

ดูขาและข้อจำกัดเซนเซอร์ใน [README หลัก](../README.md)
