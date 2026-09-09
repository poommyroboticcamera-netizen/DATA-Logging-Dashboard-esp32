"""Execute real firmware CSV code on a host, then parse it with DashboardCore.

Requires Python 3, Node.js, and g++/clang++ (native or an existing WSL distro).
No hardware, package install, project configuration change, or firmware upload.
Generated files are temporary; source extraction deliberately fails if the
firmware boundaries change, so this test cannot silently use a stale code copy.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests" / "firmware_csv"


def between(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError(f"Firmware extraction boundary changed: {start!r} / {end!r}")
    return source.split(start, 1)[1].split(end, 1)[0].join([start, ""])


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, encoding="utf-8", **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="Native g++ or clang++ executable")
    parser.add_argument("--node", help="Node.js executable")
    parser.add_argument("--wsl-distro", help="Existing WSL distro containing /usr/bin/g++")
    args = parser.parse_args()
    source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    monitor = re.search(r"constexpr bool SUPPLY_MONITOR_ENABLED = (true|false);", source)
    if not monitor:
        raise RuntimeError("Supply monitor declaration changed")
    extracted = [
        (TESTS / "host_shim.h").read_text(encoding="utf-8"),
        between(source, "struct InaReading {", "// These runtime switches"),
        between(source, "enum DeviceId {", "struct EncoderConfig"),
        between(source, "DashboardState getDashboardSnapshot() {", "\nuint8_t probeAddress("),
        monitor.group(0),
        "std::atomic<uint32_t> logIntervalMs{250}, sdDropped{0};\n",
        between(source, "struct LogRecord {", "\nQueueHandle_t sdRecords"),
        between(source, "String csvNumber(", "\nvoid logCaptureTask("),
        (TESTS / "harness.cpp").read_text(encoding="utf-8"),
    ]
    node = args.node or shutil.which("node")
    if not node:
        raise RuntimeError("Node.js not found; supply --node path/to/node")
    cxx = args.cxx or shutil.which("g++") or shutil.which("clang++")
    use_wsl = bool(args.wsl_distro or not cxx and os.name == "nt" and shutil.which("wsl"))
    if not cxx and not use_wsl:
        raise RuntimeError("No host C++ compiler found; supply --cxx or --wsl-distro")
    with tempfile.TemporaryDirectory(prefix="firmware-csv-") as temp:
        cpp = Path(temp) / "generated.cpp"
        binary = Path(temp) / ("firmware_csv.exe" if os.name == "nt" and not use_wsl else "firmware_csv")
        cpp.write_text("\n".join(extracted), encoding="utf-8")
        if use_wsl:
            prefix = ["wsl.exe"] + (["-d", args.wsl_distro] if args.wsl_distro else []) + ["--exec"]
            cpp_path = run(prefix + ["wslpath", "-u", str(cpp)], capture_output=True).stdout.strip()
            binary_path = run(prefix + ["wslpath", "-u", str(binary)], capture_output=True).stdout.strip()
            run(prefix + ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", cpp_path, "-o", binary_path])
            output = run(prefix + [binary_path], capture_output=True).stdout
        else:
            run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)])
            output = run([str(binary)], capture_output=True).stdout
        lines = output.splitlines()
        payload = {"header": lines[0], "rows": dict(line.split("|", 1) for line in lines[1:])}
        if len(payload["rows"]) != 7:
            raise AssertionError("Expected all seven native harness scenarios")
        run([node, str(TESTS / "check_packets.cjs")], input=json.dumps(payload))


if __name__ == "__main__":
    main()
