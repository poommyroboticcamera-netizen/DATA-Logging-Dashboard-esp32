#!/usr/bin/env python3
"""Bounded offline checksum hypothesis checks. No CAN interface or transmission."""
import argparse
import csv
from pathlib import Path


def crc8(data, polynomial, init, xorout, reflected=False):
    crc = init
    for value in data:
        crc ^= value
        for _ in range(8):
            if reflected:
                crc = (crc >> 1) ^ (polynomial if crc & 1 else 0)
            else:
                crc = ((crc << 1) ^ (polynomial if crc & 0x80 else 0)) & 255
    return crc ^ xorout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--id", required=True, type=lambda s: int(s, 16), help="hexadecimal ID")
    parser.add_argument("--extended", type=int, choices=(0, 1), default=0)
    parser.add_argument("--dlc", type=int, choices=range(1, 9), default=8)
    parser.add_argument("--limit", type=int, default=10000, help="maximum matching DATA rows")
    parser.add_argument("--crc8-poly", type=lambda s: int(s, 0), help="explicit polynomial, reflected representation if --reflect")
    parser.add_argument("--init", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--xorout", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--reflect", action="store_true")
    args = parser.parse_args()
    if args.limit < 32:
        parser.error("--limit must be at least 32")
    for name in ("crc8_poly", "init", "xorout"):
        value = getattr(args, name)
        if value is not None and not 0 <= value <= 255:
            parser.error(f"{name} must fit in 8 bits")
    frames = []
    malformed = 0
    with args.csv.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {"timestamp_us", "id", "extended", "dlc", *(f"d{i}" for i in range(8))}
        if not required.issubset(reader.fieldnames or []):
            parser.error("CSV header does not match analyzer format")
        for row in reader:
            try:
                if (int(row["id"], 16), int(row["extended"]), int(row["dlc"])) != (args.id, args.extended, args.dlc):
                    continue
                if all(not row[f"d{i}"] for i in range(args.dlc)):
                    continue  # RTR; .meta carries the explicit remote-frame record.
                values = [int(row[f"d{i}"]) for i in range(args.dlc)]
                if any(not 0 <= v <= 255 for v in values):
                    raise ValueError("byte range")
                frames.append(values)
            except (ValueError, TypeError, KeyError):
                malformed += 1
            if len(frames) >= args.limit:
                break
    if len(frames) < 32:
        parser.error(f"Need >=32 matching DATA frames, found {len(frames)}")
    split = len(frames) // 2
    print(f"ID 0x{args.id:X} extended={args.extended} DLC={args.dlc} n={len(frames)} malformed={malformed}")
    print("Chronological discovery/validation halves. Candidates only; no physical meanings inferred.")
    for byte in range(args.dlc):
        if len({f[byte] for f in frames}) < 4:
            continue
        algorithms = {
            "sum8": lambda d: sum(d) & 255,
            "negative_sum8": lambda d: -sum(d) & 255,
            "xor8": lambda d: xor8(d),
        }
        if args.crc8_poly is not None:
            algorithms["specified_crc8"] = lambda d: crc8(d, args.crc8_poly, args.init, args.xorout, args.reflect)
        for name, algorithm in algorithms.items():
            matches = [algorithm(f[:byte] + f[byte+1:]) == f[byte] for f in frames]
            train = sum(matches[:split]) / split
            test = sum(matches[split:]) / (len(matches)-split)
            if train >= .9:
                print(f"candidate byte={byte} hypothesis={name} discovery={train:.4f} validation={test:.4f}")
    print("Coverage assumes checksum byte excluded, remaining bytes in wire order; no ID, salt, or hidden state.")
    metadata = args.csv.with_suffix(".meta")
    if metadata.exists():
        print(f"Read loss/RTR/session context in {metadata}")


def xor8(data):
    result = 0
    for value in data:
        result ^= value
    return result


if __name__ == "__main__":
    main()
