#!/usr/bin/env python3
"""Integration regression: skip full native replacements, but never AS wrappers.

Uses a synthetic DOL, not game data. The callee contains an absolute call into
unloaded low memory like MH3's exception-vector code. Only a full replacement
may remove it from the translation graph.
"""
import json
import resource
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOTNET = ROOT / "tools/dotnet/dotnet"
CLI = ROOT / "toolchain/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="mh3-discovery-") as temp:
        root = Path(temp)
        binary = bytearray(0x118)
        for offset, value in [(0, 0x100), (0x48, 0x80001000), (0x90, 0x18), (0xE0, 0x80001000),
                              (0x100, 0x48000011), (0x104, 0x4E800020), (0x110, 0x48000063), (0x114, 0x4E800020)]:
            struct.pack_into(">I", binary, offset, value)
        (root / "main.dol").write_bytes(binary)
        (root / "native").mkdir()
        (root / "recomp.yml").write_text("""schema_version: 1
workspace_root: .
project:
  id: native-discovery-regression
memory:
  base: 0x80000000
  size: 0x01800000
  sda_base: 0x80002000
  sda2_base: 0x80003000
inputs:
  dol:
    path: main.dol
runtime:
  native_registration_root: native
  native_abi_directories: []
output:
  root: generated
""")
        cases = [
            ('REGISTER_NATIVE_FUNCTION(0x80001010, Hardware);', True),
            ('REGISTER_NATIVE_FUNCTION_AS(0x80001010, Hardware, "Hardware");', False),
            ('', False),
        ]
        for registration, should_pass in cases:
            (root / "native/hardware.cpp").write_text(
                'extern "C" void Hardware(CpuContext*) {}\n' + registration + '\n')
            result = subprocess.run([str(DOTNET), str(CLI), "translate-recursive", "0x80001000",
                                     "--project", str(root / "recomp.yml"),
                                     "--output-metadata", str(root / "result.json"), "--threads", "1"],
                                    cwd=root, capture_output=True, text=True)
            if should_pass:
                assert result.returncode == 0, result.stdout + result.stderr
                output = json.loads((root / "result.json").read_text())
                assert [f["entryPoint"] for f in output["functions"]] == [0x80001000]
                assert output["quality"]["unsupportedInstructionCount"] == 0
            else:
                assert result.returncode != 0, "Original body was silently skipped"
                assert "0x00000060" in result.stdout + result.stderr, result.stdout + result.stderr
    print("Native-discovery regression passed: replacement, wrapper, and ordinary callee")


if __name__ == "__main__":
    main()
