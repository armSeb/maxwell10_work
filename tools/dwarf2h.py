#!/usr/bin/env python3
"""Exporte enums et structs (typedef-és) d'un ELF avec DWARF vers un header C lisible."""
import sys
from elftools.elf.elffile import ELFFile

path, out = sys.argv[1], sys.argv[2]
seen = set()
enums, structs = [], []


def name(die):
    a = die.attributes.get("DW_AT_name")
    return a.value.decode() if a else None


def tname(die, depth=0):
    """Nom de type lisible."""
    if die is None or depth > 8:
        return "void"
    t = die.tag
    n = name(die)
    if t in ("DW_TAG_base_type", "DW_TAG_typedef") and n:
        return n
    if t in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_enumeration_type"):
        kw = {"DW_TAG_structure_type": "struct", "DW_TAG_union_type": "union",
              "DW_TAG_enumeration_type": "enum"}[t]
        return f"{kw} {n}" if n else f"{kw} <anon>"
    ref = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    if t == "DW_TAG_pointer_type":
        return tname(ref, depth + 1) + " *"
    if t in ("DW_TAG_const_type", "DW_TAG_volatile_type"):
        return ("const " if t == "DW_TAG_const_type" else "volatile ") + tname(ref, depth + 1)
    if t == "DW_TAG_array_type":
        dims = ""
        for c in die.iter_children():
            if c.tag == "DW_TAG_subrange_type":
                ub = c.attributes.get("DW_AT_upper_bound")
                dims += f"[{ub.value + 1 if ub else ''}]"
        return tname(ref, depth + 1) + dims
    if t == "DW_TAG_subroutine_type":
        return "fnptr"
    return n or t


def dump_enum(tdname, die):
    lines = [f"typedef enum {{  /* {tdname} */"]
    for c in die.iter_children():
        if c.tag == "DW_TAG_enumerator":
            lines.append(f"    {name(c)} = {c.attributes['DW_AT_const_value'].value},")
    lines.append(f"}} {tdname};\n")
    enums.append("\n".join(lines))


def dump_struct(tdname, die):
    size = die.attributes.get("DW_AT_byte_size")
    kw = "union" if die.tag == "DW_TAG_union_type" else "struct"
    lines = [f"typedef {kw} {{  /* {tdname}, {size.value if size else '?'} octets */"]
    for c in die.iter_children():
        if c.tag != "DW_TAG_member":
            continue
        off = c.attributes.get("DW_AT_data_member_location")
        offs = f"+0x{off.value:03x}" if off and isinstance(off.value, int) else "      "
        mt = c.get_DIE_from_attribute("DW_AT_type")
        bits = c.attributes.get("DW_AT_bit_size")
        tn = tname(mt)
        arr = ""
        if "[" in tn:
            tn, arr = tn[: tn.index("[")], tn[tn.index("["):]
        lines.append(f"    /* {offs} */ {tn} {name(c)}{arr}{' : ' + str(bits.value) if bits else ''};")
    lines.append(f"}} {tdname};\n")
    structs.append("\n".join(lines))


with open(path, "rb") as f:
    elf = ELFFile(f)
    dw = elf.get_dwarf_info()
    for cu in dw.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag != "DW_TAG_typedef" or "DW_AT_type" not in die.attributes:
                continue
            tdn = name(die)
            if not tdn or tdn in seen:
                continue
            tgt = die.get_DIE_from_attribute("DW_AT_type")
            if tgt.tag == "DW_TAG_enumeration_type":
                seen.add(tdn)
                dump_enum(tdn, tgt)
            elif tgt.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and \
                    any(True for _ in tgt.iter_children()):
                if tdn.startswith(("ST_", "U_", "PST_", "E_", "T_")) or "CMBS" in tdn or "CFR" in tdn:
                    seen.add(tdn)
                    dump_struct(tdn, tgt)

with open(out, "w") as o:
    o.write("/* Généré depuis le DWARF de cmbs_host (Gigaset Maxwell 10) */\n\n")
    o.write("/* ===== ENUMS ===== */\n\n" + "\n".join(enums))
    o.write("\n/* ===== STRUCTS ===== */\n\n" + "\n".join(structs))
print(f"{len(enums)} enums, {len(structs)} structs -> {out}")
