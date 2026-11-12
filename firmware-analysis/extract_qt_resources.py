"""Extract Qt 5 resource arrays from the local, unstripped E1 executable."""
import json
import struct
import zlib
from pathlib import Path
from elftools.elf.elffile import ELFFile

BASE = Path(__file__).resolve().parent
SOURCE = BASE / "artifacts/usr/bin/SogouE1"
OUTPUT = BASE / "extracted-resources"

with SOURCE.open("rb") as stream:
    elf = ELFFile(stream)
    symbols = list(elf.get_section_by_name(".symtab").iter_symbols())

    def contents(symbol):
        section = elf.get_section(symbol["st_shndx"])
        offset = symbol["st_value"] - section["sh_addr"]
        return section.data()[offset:offset + symbol["st_size"]]

    arrays = {}
    for kind in ("data", "name", "struct"):
        arrays[kind] = sorted(
            (s for s in symbols if s.name == f"_ZL{16 if kind != 'struct' else 18}qt_resource_{kind}"),
            key=lambda s: s["st_value"],
        )
    assert len({len(v) for v in arrays.values()}) == 1
    inventory = []
    for group in range(len(arrays["data"])):
        data, names, tree = (contents(arrays[k][group]) for k in ("data", "name", "struct"))
        assert len(tree) % 22 == 0, "Expected Qt resource format v2"
        visited = set()

        def visit(index, prefix):
            assert index not in visited
            visited.add(index)
            record = index * 22
            name_offset, flags = struct.unpack_from(">IH", tree, record)
            count = struct.unpack_from(">H", names, name_offset)[0]
            name = names[name_offset + 6:name_offset + 6 + count * 2].decode("utf-16-be")
            assert name not in ("..", ".") and "/" not in name and "\\" not in name
            path = prefix / name if index else prefix
            if flags & 2:
                child_count, child_index = struct.unpack_from(">II", tree, record + 6)
                for child in range(child_index, child_index + child_count):
                    visit(child, path)
            else:
                data_offset = struct.unpack_from(">I", tree, record + 10)[0]
                length = struct.unpack_from(">I", data, data_offset)[0]
                payload = data[data_offset + 4:data_offset + 4 + length]
                assert len(payload) == length
                if flags & 1:
                    expected = struct.unpack_from(">I", payload)[0]
                    payload = zlib.decompress(payload[4:])
                    assert len(payload) == expected
                destination = OUTPUT / f"resource-{group + 1}" / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(payload)
                inventory.append({"resource_group": group + 1, "path": str(path), "bytes": len(payload)})

        visit(0, Path())
    (OUTPUT / "inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + "\n")
    print(f"Extracted {len(inventory)} resources to {OUTPUT}")
