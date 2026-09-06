#!/usr/bin/env python3
"""Assemble a local, opt-in clangd package; no downloads or global configuration."""
import argparse
from pathlib import Path

from lsp_contract import manifest, render
from package_local import assemble as assemble_local


def assemble(binary, output):
    return assemble_local(binary, output, "clangd", lambda os_name, arch: render(manifest(os_name, arch)))


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
