#!/usr/bin/env python3
"""Install private Linux inference libraries; no system Python/CUDA changes required."""
import pathlib
import argparse
import shutil
import subprocess
import sys
import tempfile
import zipfile

PLUGIN = pathlib.Path(__file__).resolve().parents[1]
DEST = PLUGIN / "Binaries/ThirdParty/OnnxCuda/Linux"
PACKAGES = [
    "onnxruntime-gpu==1.24.4",
    "nvidia-cuda-runtime-cu12==12.8.90",
    "nvidia-cublas-cu12==12.8.4.1",
    "nvidia-cudnn-cu12==9.8.0.87",
    "nvidia-cufft-cu12==11.3.3.83",
    "nvidia-curand-cu12==10.3.9.90",
    "nvidia-nvjitlink-cu12==12.8.93",
]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, type=pathlib.Path, help="Unreal engine root (containing Engine/)")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="metahuman-cuda-") as temp:
        if not args.build_only:
            subprocess.run([
                sys.executable, "-m", "pip", "download", "--no-deps",
                "--only-binary=:all:", "--python-version", "313",
                "--implementation", "cp", "--abi", "cp313", "--dest", temp,
                *PACKAGES,
            ], check=True)
        for wheel in pathlib.Path(temp).glob("*.whl"):
            with zipfile.ZipFile(wheel) as archive:
                for name in archive.namelist():
                    if name.endswith("/"):
                        continue
                    base = pathlib.PurePosixPath(name).name
                    if ".so" in base and not base.startswith("onnxruntime_pybind"):
                        print(f"Installing {base}", flush=True)
                        with archive.open(name) as source, (DEST / base).open("wb") as target:
                            shutil.copyfileobj(source, target)
                    elif ".dist-info/licenses/" in name:
                        target = DEST / "Licenses" / wheel.stem / pathlib.PurePosixPath(name).relative_to(name.split(".dist-info/")[0] + ".dist-info/licenses")
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(archive.read(name))
    link = DEST / "libonnxruntime.so.1"
    if not link.exists():
        link.symlink_to("libonnxruntime.so.1.24.4")
    headers = args.engine / "Engine/Plugins/NNE/NNERuntimeORT/Source/ThirdParty/Onnxruntime/Internal"
    subprocess.run([
        "g++", "-std=c++17", "-O2", "-I" + str(headers), str(PLUGIN / "Resources/CudaWorker.cc"),
        "-L" + str(DEST), "-Wl,-rpath,$ORIGIN", "-l:libonnxruntime.so.1.24.4",
        "-o", str(DEST / "MetaHumanCudaWorker"),
    ], check=True)
    print(f"CUDA libraries and worker installed in {DEST}")

if __name__ == "__main__":
    main()
