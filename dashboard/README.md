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

POST: `/api/settings`, `/api/device`, `/api/encoder`, `/api/shunt`, `/api/ga`, `/api/ga-frequency`, `/api/led`

POST ใช้ form fields และ header `X-Dashboard-Request: 1`; device controls ตรวจ boot/revision เพื่อป้องกันคำสั่งเก่าทับสถานะ Header นี้ไม่ใช่ authentication

## Recording behavior

- `recording: false` แสดงค่าปัจจุบันได้ แต่ไม่เพิ่มแถวบันทึก
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
```

ดูขาและข้อจำกัดเซนเซอร์ใน [README หลัก](../README.md)
