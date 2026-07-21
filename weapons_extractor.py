#!/usr/bin/env python3
"""
Extract weapon macros from test2.lua and generate individual .lua files
for each weapon (with and without holosight). Skips 8x scopes.
"""

import re
import os
from pathlib import Path

INPUT_FILE = "/Users/aisoexe/Downloads/test2.lua"
OUTPUT_DIR = "/Users/aisoexe/macro/weapons"

# Create output directory
Path(OUTPUT_DIR).mkdir(exist_ok=True)

# Read the entire file
with open(INPUT_FILE, 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

# Find all weapon blocks by looking for "if gun ==" and "elseif gun ==" patterns
weapons_data = {}

i = 0
while i < len(lines):
    line = lines[i].strip()

    # Match "if gun == WEAPON then" or "elseif gun == WEAPON then"
    match = re.match(r'(if|elseif)\s+gun\s*==\s*(\w+)\s*then', line)

    if match:
        weapon_name = match.group(2)

        # Skip 8x scopes
        if '_8X' in weapon_name:
            i += 1
            continue

        # Collect all lines until next "elseif gun ==" or "end" at same indent
        weapon_lines = []
        i += 1
        indent_level = len(lines[i-1]) - len(lines[i-1].lstrip())

        while i < len(lines):
            current_line = lines[i]
            current_indent = len(current_line) - len(current_line.lstrip())

            # Check if we hit the next weapon block or end
            if current_indent <= indent_level:
                next_match = re.match(r'(elseif|end)\s+gun', current_line.strip())
                if next_match or current_line.strip().startswith('end'):
                    break

            weapon_lines.append(current_line)
            i += 1

        # Extract moves and sleeps in order
        weapon_body = ''.join(weapon_lines)

        # Parse MoveMouseRelative with possible multiplier
        move_pattern = r'MoveMouseRelative\s*\(\s*(-?\d+)\s*\*?\s*multiplier?\s*,\s*(-?\d+)\s*\*?\s*multiplier?\s*\)'
        sleep_pattern = r'Sleep_?for\s*\(\s*(\d+)\s*\)'

        # Get all moves and sleeps with their positions
        moves_list = []
        for m in re.finditer(move_pattern, weapon_body):
            dx, dy = int(m.group(1)), int(m.group(2))
            dx = max(-127, min(127, dx))
            dy = max(-127, min(127, dy))
            moves_list.append({'pos': m.start(), 'dx': dx, 'dy': dy, 'delay': 50})

        # Find sleeps and assign to nearest following move
        for s in re.finditer(sleep_pattern, weapon_body):
            delay = int(s.group(1))
            # Find the next move after this sleep
            for move in moves_list:
                if move['pos'] > s.start():
                    move['delay'] = max(0, delay)
                    break

        if moves_list:
            weapons_data[weapon_name] = moves_list
            print(f"✓ Parsed: {weapon_name} ({len(moves_list)} moves)")
    else:
        i += 1

print(f"\nFound {len(weapons_data)} weapons total")

# Group weapons (normal + holosight versions)
weapon_groups = {}

for weapon_name, moves in weapons_data.items():
    if weapon_name.endswith('_HOLOSIGHT'):
        base_name = weapon_name.replace('_HOLOSIGHT', '')
        sight_type = 'holosight'
    else:
        base_name = weapon_name
        sight_type = 'normal'

    if base_name not in weapon_groups:
        weapon_groups[base_name] = {}

    weapon_groups[base_name][sight_type] = [{'dx': m['dx'], 'dy': m['dy'], 'delay': m['delay']} for m in moves]

# Generate individual .lua files
print(f"\nGenerating weapon files:")
for base_name, sight_variants in sorted(weapon_groups.items()):
    for sight_type, moves in sight_variants.items():
        if sight_type == 'holosight':
            filename = f"{base_name}_holosight.lua"
            display_name = f"{base_name} (Holosight)"
        else:
            filename = f"{base_name}.lua"
            display_name = base_name

        filepath = os.path.join(OUTPUT_DIR, filename)

        with open(filepath, 'w') as f:
            f.write(f"-- {display_name} Recoil Pattern\n")
            f.write(f"-- Auto-generated from test2.lua\n\n")

            for move in moves:
                f.write(f"move({move['dx']}, {move['dy']}) sleep({move['delay']})\n")

        print(f"  ✓ {filename} ({len(moves)} steps)")

print(f"\n✅ All files saved to: {OUTPUT_DIR}")
