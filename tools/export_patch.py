#!/usr/bin/env python3
"""Export selected UE source changes against an original ZIP as a Git patch."""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--engine-tree", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()
    paths = sorted(set(args.paths))
    for relative in paths:
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or not relative.startswith("Engine/Source/"):
            parser.error(f"Expected a relative Engine/Source path: {relative}")
        if path.suffix not in (".cpp", ".h", ".cs"):
            parser.error(f"This exporter is restricted to source files: {relative}")
        if not (args.engine_tree / relative).is_file():
            parser.error(f"Source file not found: {relative}")
    if args.output.exists():
        parser.error("Output already exists; choose a new patch filename")

    with zipfile.ZipFile(args.archive) as archive:
        names = set(archive.namelist())
        versions = [name for name in names if name.endswith("Engine/Build/Build.version")]
        if len(versions) != 1:
            parser.error("Expected one engine version record in the archive")
        version = json.loads(archive.read(versions[0]).decode("utf-8-sig"))
        if tuple(version[key] for key in ("MajorVersion", "MinorVersion", "PatchVersion")) != (5, 8, 3):
            parser.error("The archive is not UE 5.8.3")
        prefix = versions[0][:-len("Engine/Build/Build.version")]
        with tempfile.TemporaryDirectory(prefix="ue583-patch-export-", dir=os.getenv("TMPDIR")) as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "--quiet", "--initial-branch=main"], cwd=root, check=True)
            existing = []
            added = []
            for relative in paths:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                if prefix + relative in names:
                    destination.write_bytes(archive.read(prefix + relative))
                    existing.append(relative)
                else:
                    added.append(relative)
            # The index is the archive baseline; no temporary commits are needed.
            if existing:
                subprocess.run(["git", "add", "--", *existing], cwd=root, check=True)
            for relative in paths:
                (root / relative).write_bytes((args.engine_tree / relative).read_bytes())
            if added:
                subprocess.run(["git", "add", "--intent-to-add", "--", *added], cwd=root, check=True)
            patch = subprocess.check_output([
                "git", "diff", "--no-ext-diff", "--no-color", "--src-prefix=a/", "--dst-prefix=b/", "--", *paths,
            ], cwd=root)
            if not patch:
                parser.error("The selected files do not differ from the archive")
            with args.output.open("xb") as output:
                output.write(patch)
    print(f"Exported selected source changes to {args.output}")


if __name__ == "__main__":
    main()
