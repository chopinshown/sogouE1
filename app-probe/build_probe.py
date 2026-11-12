"""Build a minimal unsigned Linux ARM64 ELF using LLVM and pyelftools."""
import struct
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

base = Path(__file__).resolve().parent
obj = base / "unsigned_probe.o"
subprocess.run([
    "/opt/homebrew/opt/llvm/bin/clang", "--target=aarch64-linux-gnu",
    "-c", str(base / "unsigned_probe.S"), "-o", str(obj),
], check=True)
with obj.open("rb") as stream:
    elf = ELFFile(stream)
    assert not any(s.name.startswith(".rela") and s["sh_size"] for s in elf.iter_sections())
    code = elf.get_section_by_name(".text").data()

offset, address = 0x1000, 0x400000
size = offset + len(code)
ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
header = struct.pack(
    "<16sHHIQQQIHHHHHH", ident, 2, 183, 1, address + offset,
    64, 0, 0, 64, 56, 1, 0, 0, 0,
)
segment = struct.pack("<IIQQQQQQ", 1, 5, 0, address, address, size, size, 0x1000)
destination = base / "unsigned_probe"
destination.write_bytes((header + segment).ljust(offset, b"\x00") + code)
destination.chmod(0o755)
print(destination)
