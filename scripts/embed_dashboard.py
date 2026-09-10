"""Embed the local dashboard into firmware; no separate filesystem upload."""
from pathlib import Path
import gzip


def build(root):
    root = Path(root)
    ui = root / 'dashboard'
    html = (ui / 'index.html').read_text(encoding='utf-8')
    for marker, file in [('/*__STYLE__*/', 'style.css'), ('/*__CORE__*/', 'core.js'), ('/*__MODEL__*/', 'imu-model.js'), ('/*__CAN__*/', 'can.js'), ('/*__APP__*/', 'app.js')]:
        html = html.replace(marker, (ui / file).read_text(encoding='utf-8'))
    data = gzip.compress(html.encode('utf-8'), mtime=0)
    lines = [','.join(f'0x{b:02x}' for b in data[i:i+24]) for i in range(0, len(data), 24)]
    target = root / 'include/dashboard_page.h'
    target.write_text('#pragma once\n#include <Arduino.h>\nconst uint8_t DASHBOARD_GZIP[] PROGMEM = {\n' + ',\n'.join(lines) + '\n};\n', encoding='utf-8')
    print(f'Dashboard: {len(html.encode("utf-8"))} bytes -> {len(data)} bytes gzip')


if __name__ == '__main__':
    build(Path(__file__).resolve().parents[1])
else:
    Import('env')
    build(env['PROJECT_DIR'])
