// dcb_recompiler — MIPS R3000A -> C translator (host tool).
//
// Planned inputs:  the PS-EXE, config/<id>/functions.json (exported from Ghidra by
//                  ghidra/scripts/export_functions.py), overlay descriptions.
// Planned output:  generated/<id>/*.c plus the sorted recomp_function_table.

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <boot.exe> <functions.json> [-o generated/<id>]\n", argv[0]);
        return 2;
    }
    std::fprintf(stderr, "dcb_recompiler: not implemented yet\n");
    return 1;
}
