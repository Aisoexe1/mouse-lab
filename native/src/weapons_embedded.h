#pragma once
#include <cstddef>

struct EmbeddedWeapon {
    const char*          filename;  // e.g. "ak.json"
    const unsigned char* data;      // XOR-obfuscated payload
    std::size_t          size;
};

extern const EmbeddedWeapon kEmbeddedWeapons[];
extern const int kEmbeddedWeaponCount;
