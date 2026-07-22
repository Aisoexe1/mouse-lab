#!/usr/bin/env python3
"""
Reads weapons/*.json, XOR-obfuscates each file using its own filename as
a rolling key, and emits weapons_embedded.cpp with the encrypted arrays.
Runtime decryption: XOR the data with the same filename key.
"""
import os, sys

def xor_with_key(data: bytes, key: str) -> bytes:
    kb = key.encode("utf-8")
    return bytes(b ^ kb[i % len(kb)] for i, b in enumerate(data))

def safe_ident(name: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in os.path.splitext(name)[0])

def main():
    if len(sys.argv) != 3:
        print("usage: embed_weapons.py <weapons_dir> <output.cpp>", file=sys.stderr)
        sys.exit(1)

    weapons_dir = sys.argv[1]
    output_cpp  = sys.argv[2]

    files = sorted(f for f in os.listdir(weapons_dir) if f.endswith(".json"))

    with open(output_cpp, "w") as out:
        out.write('#include "weapons_embedded.h"\n')
        out.write("// Auto-generated at configure time — do not edit\n\n")

        for fname in files:
            path = os.path.join(weapons_dir, fname)
            with open(path, "rb") as f:
                raw = f.read()
            enc = xor_with_key(raw, fname)
            ident = safe_ident(fname)
            hex_bytes = ",".join(f"0x{b:02x}" for b in enc)
            out.write(f"static const char kWD_{ident}[] = {{{hex_bytes}}};\n")

        out.write(f"\nconst EmbeddedWeapon kEmbeddedWeapons[] = {{\n")
        entries = [
            f'  {{"{fname}", kWD_{safe_ident(fname)}, sizeof(kWD_{safe_ident(fname)})}}'
            for fname in files
        ]
        out.write(",\n".join(entries))
        out.write("\n};\n")
        out.write(f"const int kEmbeddedWeaponCount = {len(files)};\n")

if __name__ == "__main__":
    main()
