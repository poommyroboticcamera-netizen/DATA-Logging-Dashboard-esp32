# Hardware design assets

This directory contains reference design exports for the ESP32 Data Logging Dashboard PCB. The current package includes a five-page schematic, a five-page PCB layout export, a STEP assembly model, and a rendered 3D preview.

![ESP32 Data Logging Dashboard PCB](3d/previews/dashboard-pcb-assembly-2026-09-08.png)

## Available design files

| Asset | Date | Format | File |
|:--|:--|:--|:--|
| Electrical schematic | 2026-09-09 | PDF, 5 pages | [Open schematic](schematic/exports/dashboard-schematic-2026-09-09.pdf) |
| PCB layout | 2026-09-09 | PDF, 5 pages | [Open PCB layout](pcb/exports/dashboard-pcb-layout-2026-09-09.pdf) |
| PCB assembly model | 2026-09-08 | STEP AP214 | [Download STEP model](3d/exports/dashboard-pcb-assembly-2026-09-08.step) |
| PCB assembly preview | 2026-09-08 | PNG | [Open preview](3d/previews/dashboard-pcb-assembly-2026-09-08.png) |

The public PCB PDF preserves the five rendered layout pages while removing 428 EasyEDA-generated JavaScript link annotations. This prevents executable PDF actions from being distributed without changing the visible PCB layers.

## Directory structure

| Folder | Purpose | Current status |
|:--|:--|:--|
| [schematic/source](schematic/source/) | Editable schematic sources and required libraries | Reserved |
| [schematic/exports](schematic/exports/) | Reviewable schematic PDF/SVG exports | PDF available |
| [pcb/source](pcb/source/) | Editable PCB layout and footprint sources | Reserved |
| [pcb/exports](pcb/exports/) | Reviewable PCB layout exports | PDF available |
| [pcb/manufacturing](pcb/manufacturing/) | Gerber, drill, BOM, and pick-and-place files | No production release |
| [3d/source](3d/source/) | Native mechanical CAD sources | Reserved |
| [3d/exports](3d/exports/) | Exchange models such as STEP and STL | STEP available |
| [3d/previews](3d/previews/) | Rendered images for documentation | PNG available |

## Revision guidance

Keep schematic, PCB, firmware pin assignments, and mechanical assets under the same revision identifier before manufacturing. Record the editor version, units, board revision, and material changes whenever a design file is replaced.

These files are design references and have not been released as a production manufacturing package. Review electrical ratings, footprints, clearances, connector pinout, mounting dimensions, and the latest firmware configuration before ordering a board.
