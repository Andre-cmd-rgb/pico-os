#!/usr/bin/env python3
"""Exercise the package manager with real files and injected I/O/PSA failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PSA = """#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t sum; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT { 0 }
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
int psa_crypto_init(void);
int psa_hash_setup(psa_hash_operation_t *, int);
int psa_hash_update(psa_hash_operation_t *, const uint8_t *, size_t);
int psa_hash_finish(psa_hash_operation_t *, uint8_t *, size_t, size_t *);
int psa_hash_abort(psa_hash_operation_t *);
"""

with tempfile.TemporaryDirectory(prefix="pico-pkg-") as tmp:
    temp = Path(tmp)
    (temp / "psa").mkdir()
    (temp / "psa/crypto.h").write_text(PSA)
    (temp / "pt").mkdir()
    (temp / "pt/kernel.h").write_text(
        "#pragma once\n#include <stdbool.h>\nbool proc_alive(int);\n"
        "bool proc_group_alive(int);\n")
    exe = str(temp / "pkg_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/pkg_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe, tmp], check=True, timeout=20)
