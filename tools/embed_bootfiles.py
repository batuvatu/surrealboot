#!/usr/bin/env python3

from pathlib import Path
import sys


#
# Keep the producer-side LZ4 block size aligned with the larger
# decompression window used by surreal_boot.c.
#
BLOCK = 128 * 1024


if len(sys.argv) != 4:
    print(
        "usage: embed_bootfiles.py <bootfiles> <output.c> <output.h>",
        file=sys.stderr,
    )
    sys.exit(1)


root = Path(sys.argv[1]).resolve()
out_c = Path(sys.argv[2]).resolve()
out_h = Path(sys.argv[3]).resolve()

blob_dir = out_c.parent / "boot_blobs"
blob_dir.mkdir(
    parents=True,
    exist_ok=True,
)


#
# Import the local compressor without requiring pip.
#
sys.path.insert(
    0,
    str(Path(__file__).resolve().parent),
)

from lz4simple import compress


files = sorted(
    p
    for p in root.rglob("*.boot")
    if p.is_file()
)


#
# No boot files:
# generate empty embedded payload tables for dynamic flash mode.
#
if not files:

    print(
        "No *.boot files found — generating empty "
        "embedded payload tables "
        "(dynamic flash payload mode)"
    )

    c_content = """#include <stdint.h>
#include "bootfiles_data.h"

const struct bootfile_desc bootfiles[] = {};
"""

    h_content = """#pragma once

#include <stdint.h>

struct bootfile_chunk {
    const uint8_t *start;
    const uint8_t *end;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

struct bootfile_desc {
    const char *name;
    const struct bootfile_chunk *chunks;
    uint16_t chunk_count;
    uint32_t uncompressed_size;
};

extern const struct bootfile_desc bootfiles[];

#define BOOTFILE_COUNT 0
"""

    out_c.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    out_c.write_text(
        c_content,
        encoding="utf-8",
    )

    out_h.write_text(
        h_content,
        encoding="utf-8",
    )

    sys.exit(0)


records = []


#
# ============================================================
# Compress boot files
# ============================================================
#

for file_index, path in enumerate(files):

    raw = path.read_bytes()

    chunks = []

    for chunk_index, offset in enumerate(
        range(
            0,
            len(raw),
            BLOCK,
        )
    ):

        block = raw[
            offset:
            offset + BLOCK
        ]

        packed = compress(block)

        blob = blob_dir / (
            f"bootfile_"
            f"{file_index:02d}_"
            f"{chunk_index:04d}.lz4"
        )

        blob.write_bytes(packed)

        chunks.append(
            (
                blob,
                len(packed),
                len(block),
            )
        )

    records.append(
        (
            path.relative_to(root).as_posix(),
            len(raw),
            chunks,
        )
    )


#
# ============================================================
# Generate C
# ============================================================
#

c = []

c.append("#include <stdint.h>")
c.append('#include "bootfiles_data.h"')
c.append("")


#
# Emit every compressed payload blob.
#
for fi, (
    _name,
    _raw_size,
    chunks,
) in enumerate(records):

    for ci, (
        blob,
        _packed,
        _raw,
    ) in enumerate(chunks):

        blob_path = (
            blob.as_posix()
            .replace("\\", "/")
        )

        c.extend(
            [
                "__asm__(",

                '    ".section .bootfiles, '
                '\\\"a\\\", %progbits\\n"',

                '    ".balign 4\\n"',

                f'    ".global '
                f'bootfile_{fi}_chunk_{ci}_start\\n"',

                f'    "'
                f'bootfile_{fi}_chunk_{ci}_start:'
                f'\\n"',

                f'    ".incbin '
                f'\\\"{blob_path}\\\"'
                f'\\n"',

                f'    ".global '
                f'bootfile_{fi}_chunk_{ci}_end'
                f'\\n"',

                f'    "'
                f'bootfile_{fi}_chunk_{ci}_end:'
                f'\\n"',

                ");",
                "",
            ]
        )


#
# Emit chunk metadata tables.
#
for fi, (
    _name,
    _raw_size,
    chunks,
) in enumerate(records):

    c.append(
        f"static const struct bootfile_chunk "
        f"bootfile_{fi}_chunks[] = {{"
    )

    for ci, (
        _blob,
        packed,
        raw,
    ) in enumerate(chunks):

        c.append(
            f"    {{"
            f"bootfile_{fi}_chunk_{ci}_start, "
            f"bootfile_{fi}_chunk_{ci}_end, "
            f"{packed}u, "
            f"{raw}u"
            f"}},"
        )

    c.extend(
        [
            "};",
            "",
        ]
    )


#
# Emit bootfile descriptors.
#
c.append(
    "const struct bootfile_desc bootfiles[] = {"
)

for fi, (
    name,
    raw_size,
    chunks,
) in enumerate(records):

    escaped_name = (
        name
        .replace("\\", "\\\\")
        .replace('"', '\\"')
    )

    c.append(
        f'    {{"{escaped_name}", '
        f'bootfile_{fi}_chunks, '
        f'{len(chunks)}u, '
        f'{raw_size}u}},'
    )

c.extend(
    [
        "};",
        "",
    ]
)

out_c.parent.mkdir(
    parents=True,
    exist_ok=True,
)

out_c.write_text(
    "\n".join(c),
    encoding="utf-8",
)


#
# ============================================================
# Generate header
# ============================================================
#

h = """#pragma once

#include <stdint.h>

struct bootfile_chunk {
    const uint8_t *start;
    const uint8_t *end;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

struct bootfile_desc {
    const char *name;
    const struct bootfile_chunk *chunks;
    uint16_t chunk_count;
    uint32_t uncompressed_size;
};

extern const struct bootfile_desc bootfiles[];

"""


for fi, (
    _name,
    _raw_size,
    chunks,
) in enumerate(records):

    for ci, _ in enumerate(chunks):

        h += (
            f"extern const uint8_t "
            f"bootfile_{fi}_chunk_{ci}_start[];\n"
        )

        h += (
            f"extern const uint8_t "
            f"bootfile_{fi}_chunk_{ci}_end[];\n"
        )


h += (
    f"\n"
    f"#define BOOTFILE_COUNT "
    f"{len(records)}\n"
)


out_h.write_text(
    h,
    encoding="utf-8",
)


#
# ============================================================
# Report
# ============================================================
#

print(
    f"Embedded {len(records)} bootfile(s) "
    f"as 128 KiB LZ4 blocks:"
)


for (
    name,
    raw_size,
    chunks,
) in records:

    compressed = sum(
        x[1]
        for x in chunks
    )

    print(
        f"  {name}: "
        f"{raw_size} -> "
        f"{compressed} bytes "
        f"in {len(chunks)} blocks"
    )
