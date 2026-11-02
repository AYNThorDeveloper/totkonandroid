#!/usr/bin/env python3
"""Generate private C loader tables from this user's prepared TOTK 1.4.3 modules."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "games/totk"
sys.path.insert(0, str(GAME / "third_party/d2r"))
from relr import decode_relr


def read_layout(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith("# "):
            key, value = line[2:].split("\t", 1)
            if key in {"PATCH_OFF", "KEY_PAGE_OFF", "KEY_PAGE_SIZE", "TEXT_END", "RO_OFF", "DATA_OFF"}:
                result[key] = int(value, 0)
    required = {"PATCH_OFF", "KEY_PAGE_OFF", "KEY_PAGE_SIZE", "TEXT_END", "RO_OFF", "DATA_OFF"}
    if not required.issubset(result):
        raise ValueError(f"incomplete fixup layout: {path}")
    return result


def read_tags(image):
    modoff = struct.unpack_from("<I", image, 4)[0]
    if image[modoff:modoff + 4] != b"MOD0":
        raise ValueError("module has no MOD0 header")
    dyn = modoff + struct.unpack_from("<i", image, modoff + 4)[0]
    tags = {}
    for i in range(4096):
        tag, value = struct.unpack_from("<qQ", image, dyn + 16 * i)
        if tag == 0:
            return tags
        tags[tag] = value
    raise ValueError("unterminated dynamic table")


def prepare_module(name, version, rewritten, extracted):
    image_path = rewritten / f"{name}.patched.bin"
    image = image_path.read_bytes()
    layout = read_layout(rewritten / f"{name}.fixups.tsv")
    tags = read_tags(image)
    if tags.get(11) != 24 or tags.get(9) != 24 or 6 not in tags or 5 not in tags:
        raise ValueError(f"unexpected ELF dynamic table in {name}")
    if 4 in tags:
        nsym = struct.unpack_from("<I", image, tags[4] + 4)[0]
    else:
        nso = (extracted / name).read_bytes()
        _, _, sym_off, sym_size = struct.unpack_from("<IIII", nso, 0x90)
        if layout["RO_OFF"] + sym_off != tags[6] or sym_size % 24:
            raise ValueError(f"cannot establish dynamic symbol count in {name}")
        nsym = sym_size // 24
    strings = image[tags[5]:tags[5] + tags[10]]
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = struct.unpack_from("<IBBHQQ", image, tags[6] + 24 * i)
        start = no
        end = strings.find(b"\0", start)
        if end < 0:
            raise ValueError(f"bad symbol name in {name}")
        symbol_name = strings[start:end].decode("utf-8")
        symbols.append(dict(name=symbol_name, value=value, size=size,
                             defined=bool(section), binding=info >> 4, type=info & 15))
    relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
              for where, addend in decode_relr(image, tags, layout["DATA_OFF"], layout["KEY_PAGE_OFF"])]
    seen = {r["where"] for r in relocs}
    for address, size in ((tags.get(7, 0), tags.get(8, 0)), (tags.get(23, 0), tags.get(2, 0))):
        if size % 24:
            raise ValueError(f"bad RELA size in {name}")
        for off in range(address, address + size, 24):
            where, info, addend = struct.unpack_from("<QQq", image, off)
            kind, symbol = info & 0xFFFFFFFF, info >> 32
            if kind not in (257, 1025, 1026, 1027) or symbol >= nsym:
                raise ValueError(f"unsupported relocation in {name} at {off:#x}")
            if not layout["DATA_OFF"] <= where <= layout["KEY_PAGE_OFF"] - 8 or where in seen:
                raise ValueError(f"invalid or duplicate relocation target in {name} at {where:#x}")
            if kind == 1027 and not 0 <= addend < layout["KEY_PAGE_OFF"]:
                raise ValueError(f"bad relative relocation addend in {name}")
            seen.add(where)
            relocs.append(dict(where=where, kind=kind, symbol=symbol, addend=addend))
    gates = []
    for line in (rewritten / f"{name}.gates.tsv").read_text().splitlines():
        if not line.strip():
            continue
        ret, kind, arg, original = line.split()
        if kind not in ("cntpct", "cntvct", "svc"):
            raise ValueError(f"unknown gate kind {kind}")
        gates.append(dict(ret=int(ret, 0), timer=kind != "svc", arg=int(arg), original=int(original, 0)))
    init = tags.get(12, 0)
    load_offset = 20 if version == "v143" else 24
    flag_reg = 19 if version == "v143" else 20
    adrp = struct.unpack_from("<I", image, init + 16)[0]
    load = struct.unpack_from("<I", image, init + load_offset)[0]
    if adrp & 0x9F00001F != 0x90000000 | flag_reg or load & 0xFFC003FF != 0xB9400008 | (flag_reg << 5):
        raise ValueError(f"unexpected constructor flag sequence in {name}")
    imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    ctor_flag = ((init + 16) & ~4095) + (imm << 12) + ((load >> 10) & 0xFFF) * 4
    if not layout["DATA_OFF"] <= ctor_flag < layout["KEY_PAGE_OFF"]:
        raise ValueError(f"constructor flag outside data in {name}")
    return dict(name=name, digest=hashlib.sha256(image).hexdigest(), file_size=len(image),
                text_end=layout["TEXT_END"], ro_off=layout["RO_OFF"],
                data_off=layout["DATA_OFF"], patch_off=layout["PATCH_OFF"],
                key_off=layout["KEY_PAGE_OFF"], init=init, ctor_flag=ctor_flag,
                init_array=tags.get(25, 0), init_size=tags.get(27, 0),
                symbols=symbols, relocs=relocs, gates=gates)


def cstr(value):
    out = ['"']
    for byte in value.encode("utf-8"):
        if byte == 0x22:
            out.append('\\"')
        elif byte == 0x5c:
            out.append('\\\\')
        elif 0x20 <= byte < 0x7f:
            out.append(chr(byte))
        else:
            out.append(f"\\{byte:03o}")
    out.append('"')
    return "".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="v143", choices=("v143",))
    parser.add_argument("--output", type=Path, default=ROOT / "android/build/android_runtime_tables.h")
    args = parser.parse_args()
    version = args.version
    extraction = GAME / f"extracted/{version}"
    rewritten = GAME / f"rewritten/android-{version}"
    manifest = json.loads((extraction / "extraction-manifest.json").read_text())
    expected = {entry["name"]: entry["sha256"] for entry in manifest["files"]}
    for name in ("main", "sdk", "subsdk0"):
        if hashlib.sha256((extraction / "exefs" / name).read_bytes()).hexdigest() != expected[name]:
            raise ValueError(f"extracted {name} differs from its ExeFS manifest")
    source_hash = hashlib.sha256((GAME / "third_party/d2r/patch_nso.py").read_bytes()).hexdigest()
    recorded_hash = (rewritten / "build_src.sha256").read_text().split()[0]
    if source_hash != recorded_hash:
        raise ValueError("Android rewrites are stale; rerun patch_nso.py with DM_THREAD_POINTER_REGISTER=tpidr_el0")
    modules = [prepare_module(name, version, rewritten, extraction / "exefs")
               for name in ("main", "subsdk0", "sdk")]
    lines = ["/* Generated from this user's verified TOTK 1.4.3 files; do not commit. */"]
    for n, module in enumerate(modules):
        lines.append(f"static const Reloc android_reloc_{n}[] = {{")
        lines.extend(f"{{{r['where']}ULL,{r['kind']},{r['symbol']},{r['addend']}LL}}," for r in module["relocs"])
        lines.append("};")
        lines.append(f"static const Symbol android_symbol_{n}[] = {{")
        lines.extend("{" + f"{cstr(s['name'])},{s['value']}ULL,{s['size']}ULL,{int(s['defined'])},{s['binding']},{s['type']}" + "},"
                     for s in module["symbols"])
        lines.append("};")
        lines.append(f"static const Gate android_gate_{n}[] = {{")
        lines.extend(f"{{{g['ret']}ULL,{g['original']}ULL,{int(g['timer'])},{g['arg']}}}," for g in module["gates"])
        if not module["gates"]:
            lines.append("{0,0,0,0},")
        lines.append("};")
    lines.append("static Module android_modules[] = {")
    for n, m in enumerate(modules):
        lines.append("{" + f".name={cstr(m['name'])},.digest={cstr(m['digest'])},.file_size={m['file_size']}ULL,"
                     f".text_end={m['text_end']}ULL,.ro_off={m['ro_off']}ULL,.data_off={m['data_off']}ULL,"
                     f".patch_off={m['patch_off']}ULL,.key_off={m['key_off']}ULL,.symbols=android_symbol_{n},"
                     f".nsym={len(m['symbols'])},.relocs=android_reloc_{n},.nreloc={len(m['relocs'])},"
                     f".gates=android_gate_{n},.ngate={len(m['gates'])},.init={m['init']}ULL,"
                     f".ctor_flag={m['ctor_flag']}ULL,.init_array={m['init_array']}ULL,.init_size={m['init_size']}ULL" + "},")
    lines.extend(("};", "#define ANDROID_MODULE_COUNT (sizeof(android_modules) / sizeof(android_modules[0]))"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")
    print(json.dumps({"version": version, "modules": len(modules),
                      "symbols": sum(len(m["symbols"]) for m in modules),
                      "relocations": sum(len(m["relocs"]) for m in modules),
                      "gates": sum(len(m["gates"]) for m in modules),
                      "output": str(args.output)}))


if __name__ == "__main__":
    main()
