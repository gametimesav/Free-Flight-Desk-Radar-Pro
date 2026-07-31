import os
import subprocess
import sys
from pathlib import Path

try:
    Import("env")  # type: ignore[name-defined]
except Exception:  # pragma: no cover
    env = None


def find_workspace_python(project_dir: Path) -> str:
    candidates = [
        project_dir / ".venv" / "bin" / "python",
        project_dir / ".venv" / "Scripts" / "python.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return sys.executable


def find_esptool_python(python_exe: str) -> str:
    home = Path.home()
    candidates = [
        home / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py",
        home / ".platformio" / "penv" / "bin" / "esptool.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return ""


def create_combined_image(build_dir: Path, output_path: Path) -> None:
    project_dir = build_dir.parent.parent.parent if build_dir.name == "esp32dev" else build_dir.parent.parent
    python_exe = find_workspace_python(project_dir)
    esptool_path = find_esptool_python(python_exe)
    if not esptool_path:
        raise RuntimeError("Could not find esptool.py in the PlatformIO installation")

    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    app = build_dir / "firmware.bin"
    littlefs = build_dir / "littlefs.bin"

    missing = [str(path) for path in [bootloader, partitions, app, littlefs] if not path.exists()]
    if missing:
        raise FileNotFoundError(f"Missing expected build artifacts: {missing}")

    cmd = [
        python_exe,
        esptool_path,
        "--chip",
        "esp32",
        "merge_bin",
        "-o",
        str(output_path),
        "0x1000",
        str(bootloader),
        "0x8000",
        str(partitions),
        "0x10000",
        str(app),
        "0x310000",
        str(littlefs),
    ]
    print("Running:", " ".join(cmd))
    subprocess.run(cmd, cwd=project_dir, check=True)
    print(f"Created combined image: {output_path}")


def merge_firmware(source, target, env_obj):
    build_dir = Path(env_obj.subst("$BUILD_DIR"))
    output_path = build_dir / "firmware-combined.bin"
    create_combined_image(build_dir, output_path)


if env is not None:
    env.AddPostAction("$BUILD_DIR/firmware.bin", merge_firmware)
else:
    project_dir = Path(__file__).resolve().parent.parent
    build_dir = project_dir / ".pio" / "build" / "esp32dev"
    output_path = build_dir / "firmware-combined.bin"
    create_combined_image(build_dir, output_path)
