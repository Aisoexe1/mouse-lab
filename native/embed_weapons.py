#!/usr/bin/env python3
"""
Reads weapons/*.json and emits weapons_embedded.cpp with three-layer
XOR-obfuscated arrays:
  Layer 1 – fixed 8-byte key XOR
  Layer 2 – filename-derived key XOR  (char * 0x1B + 0x37)
  Layer 3 – nibble swap (involution — same op reverses it)
Runtime: reverse layers 3 → 2 → 1 inside DecryptWeapon().
"""
import os, sys

_K8 = [0x9E, 0x37, 0x79, 0xB9, 0x7F, 0x4A, 0x7C, 0x15]

def _derive_fk(fname: str) -> bytes:
    return bytes(((ord(c) * 0x1B) + 0x37) & 0xFF for c in fname)

def encrypt(data: bytes, fname: str) -> bytes:
    fk = _derive_fk(fname)
    r = bytes(b ^ _K8[i % 8]       for i, b in enumerate(data))   # L1
    r = bytes(b ^ fk[i % len(fk)]  for i, b in enumerate(r))       # L2
    r = bytes(((b << 4) | (b >> 4)) & 0xFF for b in r)             # L3
    return r

def safe_ident(name: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in os.path.splitext(name)[0])

def main():
    if len(sys.argv) != 3:
        print("usage: embed_weapons.py <weapons_dir> <output.cpp>", file=sys.stderr)
        sys.exit(1)

    weapons_dir, output_cpp = sys.argv[1], sys.argv[2]
    files = sorted(f for f in os.listdir(weapons_dir) if f.endswith(".json"))

    with open(output_cpp, "w") as out:
        out.write('#include "weapons_embedded.h"\n')
        out.write("// Auto-generated at configure time — do not edit\n\n")
        for fname in files:
            with open(os.path.join(weapons_dir, fname), "rb") as f:
                raw = f.read()
            enc   = encrypt(raw, fname)
            ident = safe_ident(fname)
            out.write(f"static const unsigned char kWD_{ident}[] = "
                      f"{{{','.join(f'0x{b:02x}' for b in enc)}}};\n")
        out.write(f"\nconst EmbeddedWeapon kEmbeddedWeapons[] = {{\n")
        entries = [
            f'  {{"{f}", kWD_{safe_ident(f)}, sizeof(kWD_{safe_ident(f)})}}'
            for f in files
        ]
        out.write(",\n".join(entries) + "\n};\n")
        out.write(f"const int kEmbeddedWeaponCount = {len(files)};\n")

if __name__ == "__main__":
    main()
