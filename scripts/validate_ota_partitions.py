#!/usr/bin/env python3
import configparser
import csv
from pathlib import Path

project_dir = Path(__file__).resolve().parent.parent
platformio_ini = project_dir / "platformio.ini"
if not platformio_ini.exists():
    raise SystemExit("platformio.ini not found")

config = configparser.ConfigParser()
config.read(platformio_ini)
partition_name = config.get("env:esp32dev", "board_build.partitions", fallback="")
partition_file = project_dir / partition_name
if not partition_file.exists():
    raise SystemExit(f"Configured partition file not found: {partition_file}")

with partition_file.open(newline="") as fh:
    rows = list(csv.reader(fh))

header = [cell.strip() for cell in rows[0]]
parsed = []
for row in rows[1:]:
    if not row or not any(cell.strip() for cell in row):
        continue
    if row[0].startswith("#"):
        continue
    parsed.append(dict(zip(header, row)))

app_slots = [row for row in parsed if row.get("Type", "").strip() == "app"]
subtypes = {row.get("SubType", "").strip() for row in app_slots}

print(f"Partition file: {partition_file.name}")
print(f"Found app slots: {sorted(subtypes)}")

if "ota_0" not in subtypes or "ota_1" not in subtypes:
    raise SystemExit("OTA partition table must define both ota_0 and ota_1 slots")
