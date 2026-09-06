#!/usr/bin/env python3
"""Assemble a local, opt-in clangd package; no downloads or global configuration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import tempfile

from lsp_contract import manifest, render


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assemble(binary, output):
    binary = binary.resolve(strict=True)
    os_name = {"Linux": "linux", "Darwin": "macos"}.get(platform.system())
    arch = {"x86_64": "x86_64", "aarch64": "arm64", "arm64": "arm64"}.get(platform.machine())
    if not os_name or not arch or not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("An executable for the current supported POSIX platform is required")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError("Choose a new package directory; existing packages are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".clangd-stage-", dir=output.parent) as staging:
        package = Path(staging) / "clangd"
        (package / "bin").mkdir(parents=True)
        copied = package / "bin" / "clangd"
        shutil.copyfile(binary, copied)
        copied.chmod(0o755)
        digest = sha256(copied)
        if digest != sha256(binary):
            raise ValueError("Executable changed while being copied")
        (package / "manifest.xcdn").write_text(render(manifest(os_name, arch)) + "\n", encoding="utf-8")
        (package / "provenance.json").write_text(json.dumps({"source": str(binary), "sha256": digest,
            "platform": f"{os_name}-{arch}", "dependencies_bundled": False}, indent=2) + "\n", encoding="utf-8")
        # Reserve a new destination. Register the package only after assembly succeeds.
        output.mkdir()
        try:
            for child in package.iterdir():
                child.rename(output / child.name)
        except BaseException:
            shutil.rmtree(output)
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(assemble(args.binary, args.output))
    except (OSError, ValueError) as error:
        parser.exit(1, f"package_clangd: {error}\n")


if __name__ == "__main__":
    main()
