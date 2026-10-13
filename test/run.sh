#!/bin/bash
# Builds and runs the unit tests on the host (no PS5 or SDK needed): bash test/run.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/test
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} | sed 's/^unsigned/static const unsigned/' > build/test/pdb_template.h  # included by build.c and unit.c
${CC:-cc} -O1 -g -Wall -Wextra -Ibuild/test -Isrc -o build/test/unit test/unit.c src/build.c src/crypto.c src/pkg.c
build/test/unit
