#!/usr/bin/env python3
"""Check the patch series against a source ZIP without changing the real engine."""

import argparse
import difflib
import json
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import zipfile


def patch_paths(patches):
    paths = set()
    for patch in patches:
        for line in patch.read_text(encoding="utf-8").splitlines():
            if line.startswith("--- a/"):
                path = PurePosixPath(line[len("--- a/"):])
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"Unexpected patch path: {path}")
                paths.add(path.as_posix())
    return sorted(paths)


def apply_series(root, patches, ignore_space_change=False):
    for patch in patches:
        command = ["git", "apply"]
        if ignore_space_change:
            command.append("--ignore-space-change")
        subprocess.run([*command, "--check", str(patch)], cwd=root, check=True)
        subprocess.run([*command, str(patch)], cwd=root, check=True)
        print(f"PASS: {patch.name}", flush=True)


def comparable_text(path):
    # Ignore BOM/line-ending and trailing blank-line differences from extraction/editors.
    return path.read_text(encoding="utf-8-sig").rstrip("\n") + "\n"


def compare_tree(root, paths, expected_root):
    for relative in paths:
        actual = comparable_text(root / relative)
        expected = comparable_text(expected_root / relative)
        if actual != expected:
            diff = "".join(difflib.unified_diff(
                actual.splitlines(keepends=True), expected.splitlines(keepends=True),
                fromfile=f"patched/{relative}", tofile=f"expected/{relative}",
            ))
            raise RuntimeError(f"Patch does not reproduce the local source:\n{diff}")
    print(f"PASS: {len(paths)} patched files match the supplied engine tree", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--compare-tree", type=Path)
    parser.add_argument("--custom-plugin-baseline", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    patches = sorted((repo / "patches").glob("000*.patch"))
    paths = patch_paths(patches)
    if not patches or not paths:
        raise RuntimeError("No core patches found")

    with zipfile.ZipFile(args.archive) as archive:
        versions = [name for name in archive.namelist() if name.endswith("Engine/Build/Build.version")]
        if len(versions) != 1:
            raise RuntimeError("Expected exactly one Engine/Build/Build.version in the ZIP")
        prefix = versions[0][:-len("Engine/Build/Build.version")]
        version = json.loads(archive.read(versions[0]).decode("utf-8-sig"))
        if tuple(version[key] for key in ("MajorVersion", "MinorVersion", "PatchVersion")) != (5, 8, 3):
            raise RuntimeError("The archive is not UE 5.8.3")
        with tempfile.TemporaryDirectory(prefix="ue583-patch-check-") as temporary:
            root = Path(temporary)
            for relative in paths:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.read(prefix + relative))
            apply_series(root, patches)
            if args.compare_tree:
                compare_tree(root, paths, args.compare_tree)

            if args.custom_plugin_baseline:
                optional = [repo / "patches/optional/custom-plugins-5.8.patch"]
                optional_paths = patch_paths(optional)
                for relative in optional_paths:
                    destination = root / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes((args.custom_plugin_baseline / relative).read_bytes())
                # The custom plugin descriptors were originally Windows/CRLF files.
                apply_series(root, optional, ignore_space_change=True)
                if args.compare_tree:
                    compare_tree(root, optional_paths, args.compare_tree)

    print("Patch verification succeeded.")


if __name__ == "__main__":
    main()
