# Troubleshooting

## เว็บไม่เชื่อมต่อ

ใช้ IP ที่ Serial 115200 รายงานจริง อยู่เครือข่ายเดียวกัน ไม่ใช้ localhost แทน IP บอร์ด หลัง Upload ให้ Ctrl+F5

## ปุ่มเริ่มไม่ทำงาน

ตรวจ runtime_config.h ปัจจุบัน START=GPIO36, STOP=GPIO39 กดเป็น LOW ใช้ pull-up ภายนอก ถ้า STOP ค้าง LOW ระบบให้หยุดก่อน api/health รายงานขาที่ตั้ง ระดับ GPIO34/36/39 จำนวนการกด และ recording เพื่อเทียบการต่อจริง

## รีเซ็ตเมื่อกดปุ่มหรือสั่น

เก็บ reset/Backtrace ก่อนถอดไฟ และอ่าน /api/health ทันที

- INTERRUPT_WATCHDOG: ต้องใช้ Backtrace หาจุดที่ interrupt ถูกขวาง
- POWER_ON: ตรวจ EN/Reset, ไฟเลี้ยงและหน้าสัมผัส ไม่สรุปว่า CPU ทำงานหนัก
- EN ต้องเป็น HIGH ขณะกดปุ่มบันทึก GPIO ถ้าลง LOW ชิปจะรีเซ็ตทางฮาร์ดแวร์

## DS18B20 ไม่พบ / DHT22 ไม่ออก

DS ใช้ DATA แยก GPIO27/16/13/14; DHT22 ใช้ GPIO4 ตรวจ 3.3V, GND, pull-up และสถานะเปิดอุปกรณ์ ซี 100 nF บน DATA ใน schematic รุ่นก่อนอาจทำให้ขอบสัญญาณช้า ต้องตรวจวงจรจริงและรูปคลื่น ไม่สรุปว่าต้องเพิ่มไฟเป็น 5V

DHT อ่านประมาณ 2.5 s ค่าที่อ่านพลาดไม่ปรับ timestamp ค่าดีล่าสุด เมื่อเกิน 6.5 s จะไม่ใช้เป็นข้อมูลสด

## SD เปิดไม่ได้

1. เปิด ENABLE_SD_LOGGING และ SD บน dashboard
2. ใช้ FAT16/FAT32; exFAT/NTFS ยังไม่รองรับ
3. กดเริ่ม แล้วดู sd_status, sd_mounted, sd_written
4. WAITING_FOR_START ยังไม่ใช่การตรวจพบการ์ด; MOUNT_OR_CREATE_FAILED ยังแยกไม่ได้ว่าเป็น filesystem, wiring หรือสร้างไฟล์
5. สำรองข้อมูลก่อนฟอร์แมต เลือกไดรฟ์ให้ตรงการ์ดจริง

## พอร์ตถูกใช้งาน

ปิด Serial Monitor หรือโปรแกรมอื่นที่เปิดพอร์ต แล้ว Upload ใหม่ เลือก COM ตามเครื่องปัจจุบัน
