# Hardware design assets

พื้นที่สำหรับ schematic, PCB และโมเดล 3D ในอนาคต ยังไม่มีไฟล์ออกแบบหรือชุดไฟล์ผลิตที่ตรวจสอบแล้ว

| Folder | เนื้อหา |
|---|---|
| [schematic/source](schematic/source/) | ต้นฉบับวงจร EasyEDA / KiCad |
| [schematic/exports](schematic/exports/) | PDF / SVG |
| [pcb/source](pcb/source/) | PCB layout ต้นฉบับ |
| [pcb/manufacturing](pcb/manufacturing/) | Gerber, drill, BOM, pick-and-place |
| [3d/source](3d/source/) | Native CAD |
| [3d/exports](3d/exports/) | STEP / STL |
| [3d/previews](3d/previews/) | ภาพประกอบโมเดล |

ใช้ revision เดียวกันระหว่างวงจร PCB และ CAD เช่น rev-a/ rev-b/ เก็บ source ที่แก้ไขได้ควบคู่กับ exports ระบุหน่วย เวอร์ชันโปรแกรม และหมายเหตุเมื่อเปลี่ยน pin assignment ตรวจชุดไฟล์ก่อนผลิต

ไฟล์ขนาดใหญ่ควรพิจารณา Git LFS เมื่อกำหนดรูปแบบไฟล์ที่จะใช้แล้ว
