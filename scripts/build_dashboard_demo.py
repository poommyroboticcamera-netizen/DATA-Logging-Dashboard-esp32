"""Build the self-contained, browser-only dashboard demo for GitHub Pages."""

from argparse import ArgumentParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "dashboard"
MOCK_API = ROOT / "scripts" / "dashboard_demo_mock.js"


def replace_once(html: str, marker: str, content: str) -> str:
    if html.count(marker) != 1:
        raise RuntimeError(f"Expected one {marker} marker in dashboard/index.html")
    return html.replace(marker, content)


def build(output: Path) -> None:
    html = (UI / "index.html").read_text(encoding="utf-8")
    html = replace_once(html, "/*__STYLE__*/", (UI / "style.css").read_text(encoding="utf-8"))
    html = replace_once(html, "/*__CORE__*/", (UI / "core.js").read_text(encoding="utf-8"))
    html = replace_once(html, "/*__MODEL__*/", (UI / "imu-model.js").read_text(encoding="utf-8"))
    html = replace_once(html, "/*__CAN__*/", (UI / "can.js").read_text(encoding="utf-8"))
    demo_app = MOCK_API.read_text(encoding="utf-8") + "\n" + (UI / "app.js").read_text(encoding="utf-8")
    html = replace_once(html, "/*__APP__*/", demo_app)
    html = html.replace(
        "<title>ESP32 Dashboard · Live Monitor</title>",
        '<title>ESP32 Dashboard · Interactive Demo</title><meta name="description" content="Interactive browser demo of the ESP32 Data Logging Dashboard">',
        1,
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(html, encoding="utf-8", newline="\n")
    print(f"Dashboard demo: {output} ({len(html.encode('utf-8'))} bytes)")


def main() -> None:
    parser = ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build(args.output.resolve())


if __name__ == "__main__":
    main()
