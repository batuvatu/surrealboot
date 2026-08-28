// lz4.js
// LZ4 block compressor ported from surrealboot's tools/lz4simple.py
// Matches the exact format expected by surreal_boot.c's lz4_decompress_block()
//
// Key: simple greedy hash-table match, no frame format.
// Maximum match distance: 65535. Minimum match: 4.

const LZ4_WINDOW = 65535;
const LZ4_MIN_MATCH = 4;

function _emitLen(out, length) {
    while (length >= 255) {
        out.push(255);
        length -= 255;
    }
    out.push(length);
}

function _emitSequence(out, literals, offset, matchLength) {
    const litLen = literals.length;

    const tokenLit = Math.min(litLen, 15);
    const tokenMatch = Math.min(Math.max(matchLength - 4, 0), 15);

    out.push((tokenLit << 4) | tokenMatch);

    if (litLen >= 15) {
        _emitLen(out, litLen - 15);
    }

    for (let i = 0; i < litLen; i++) {
        out.push(literals[i]);
    }

    if (offset === null) {
        return;
    }

    out.push(offset & 0xFF);
    out.push((offset >> 8) & 0xFF);

    if (matchLength - 4 >= 15) {
        _emitLen(out, matchLength - 4 - 15);
    }
}

function lz4CompressBlock(data) {
    if (!data || data.length === 0) {
        return new Uint8Array([0]);
    }

    const out = [];
    let pos = 0;
    let anchor = 0;
    const size = data.length;

    // Simple hash table (matches Python version exactly)
    const table = new Map();

    function hash4(p) {
        const x = (data[p] | (data[p + 1] << 8) | (data[p + 2] << 16) | (data[p + 3] << 24)) >>> 0;
        return ((Math.imul(x, 2654435761) >>> 0) >> 16) & 0xFFFF;
    }

    while (pos + LZ4_MIN_MATCH <= size) {
        const h = hash4(pos);
        const previous = table.get(h);
        table.set(h, pos);

        if (previous === undefined) {
            pos++;
            continue;
        }

        const distance = pos - previous;

        if (distance > LZ4_WINDOW) {
            pos++;
            continue;
        }

        // Check 4-byte match
        if (data[previous] !== data[pos] ||
            data[previous + 1] !== data[pos + 1] ||
            data[previous + 2] !== data[pos + 2] ||
            data[previous + 3] !== data[pos + 3]) {
            pos++;
            continue;
        }

        let matchLen = LZ4_MIN_MATCH;
        const maxLen = size - pos;

        while (matchLen < maxLen && data[previous + matchLen] === data[pos + matchLen]) {
            matchLen++;
        }

        if (matchLen < LZ4_MIN_MATCH) {
            pos++;
            continue;
        }

        const literals = data.slice(anchor, pos);
        _emitSequence(out, literals, distance, matchLen);

        const end = pos + matchLen;

        // Add positions covered by the match to improve subsequent matches
        let p = pos + 1;
        while (p + LZ4_MIN_MATCH <= end) {
            table.set(hash4(p), p);
            p++;
        }

        pos = end;
        anchor = pos;
    }

    // Final literal-only sequence
    const remaining = data.slice(anchor);
    if (remaining.length > 0) {
        _emitSequence(out, remaining, null, 0);
    }

    return new Uint8Array(out);
}

// Compress a full payload into surrealboot's chunked format
// Splits into 64KB blocks, each compressed independently
// Returns a binary blob with header + chunk table + compressed data
function compressPayload(data, progressCallback) {
    const CHUNK_SIZE = 65536;
    const chunkCount = Math.ceil(data.length / CHUNK_SIZE);

    const compressedChunks = [];
    let totalCompressedSize = 0;

    for (let i = 0; i < chunkCount; i++) {
        const start = i * CHUNK_SIZE;
        const end = Math.min(start + CHUNK_SIZE, data.length);
        const chunk = data.slice(start, end);
        const comp = lz4CompressBlock(chunk);

        compressedChunks.push({
            uncompressed: chunk.length,
            compressed: comp.length,
            data: comp
        });
        totalCompressedSize += comp.length;

        if (progressCallback) {
            progressCallback((i + 1) / chunkCount);
        }
    }

    // Build header:
    //   uint32 magic      "SBPT" (0x53425054)
    //   uint32 version    1
    //   uint32 uncompressed_size
    //   uint16 chunk_count
    //   uint16 reserved
    //   [chunk_count × { uint32 compressed_size, uint32 uncompressed_size }]
    //   [compressed data...]

    const headerSize = 16 + (chunkCount * 8);
    const result = new Uint8Array(headerSize + totalCompressedSize);
    const view = new DataView(result.buffer);

    view.setUint32(0, 0x53425054, true);   // magic 'SBPT'
    view.setUint32(4, 1, true);             // version
    view.setUint32(8, data.length, true);   // uncompressed_size
    view.setUint16(12, chunkCount, true);   // chunk_count
    view.setUint16(14, 0, true);            // reserved

    let offset = 16;
    for (let i = 0; i < chunkCount; i++) {
        view.setUint32(offset, compressedChunks[i].compressed, true);
        view.setUint32(offset + 4, compressedChunks[i].uncompressed, true);
        offset += 8;
    }

    for (let i = 0; i < chunkCount; i++) {
        result.set(compressedChunks[i].data, offset);
        offset += compressedChunks[i].compressed;
    }

    return {
        data: result,
        uncompressedSize: data.length,
        compressedSize: totalCompressedSize,
        totalSize: result.length,
        chunkCount: chunkCount
    };
}

window.compressPayload = compressPayload;
window.lz4CompressBlock = lz4CompressBlock;
