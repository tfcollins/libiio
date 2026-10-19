#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the VITA 49.2 golden vectors used by the DAQIRI backend tests.

Usage: gen_golden.py <outdir>

Writes the *.bin vectors, manifest.json and expected.h (the C table the
parser test compiles in) into <outdir>. Output is deterministic.

Payload pattern "ramp16": sample n has I = n & 0x7FFF and
Q = (0x8000 - n) & 0xFFFF, each packed as a big-endian 16-bit word, I first.
Payload pattern "zero": all zero bytes.

Prologue word 0 (big-endian):
type[31:28] C[27] T[26] rsv[25] TSM[24] TSI[23:22] TSF[21:20]
pkt_count[19:16] size[15:0]; then Stream ID, Class ID (2 words), TSI, TSF
(2 words), each present only when signalled in word 0.
"""
import json
import os
import struct
import sys

STREAM_ID = 0x01000000
OUI = 0x000456
INFO_CLASS = 0
PACKET_CLASS = 1  # CI16
TSI_SECS = 1700000000
SAMPLES = 360
TRAILER = 0x00000000


def ramp16(nsamples):
    out = bytearray()
    for n in range(nsamples):
        out += struct.pack('>HH', n & 0x7FFF, (0x8000 - n) & 0xFFFF)
    return bytes(out)


def build(ptype, pkt_count, payload, has_class=False, tsi=0, tsf=0,
          tsf_frac=0, trailer=False, size_override=None):
    """Return (bytes, manifest-entry-without-name) for one packet."""
    has_sid = ptype in (1, 3) or ptype >= 4
    words = []
    if has_sid:
        words.append(STREAM_ID)
    if has_class:
        words.append(OUI & 0xFFFFFF)
        words.append((INFO_CLASS << 16) | PACKET_CLASS)
    if tsi:
        words.append(TSI_SECS)
    if tsf:
        words.append(tsf_frac >> 32)
        words.append(tsf_frac & 0xFFFFFFFF)
    prologue_bytes = 4 + 4 * len(words)
    size_words = (prologue_bytes + len(payload)) // 4 + (1 if trailer else 0)
    w0 = ((ptype << 28) | (int(has_class) << 27) | (int(trailer) << 26) |
          (tsi << 22) | (tsf << 20) | (pkt_count << 16))
    w0 |= (size_override if size_override is not None else size_words)
    data = struct.pack('>I', w0)
    data += b''.join(struct.pack('>I', w) for w in words)
    data += payload
    if trailer:
        data += struct.pack('>I', TRAILER)
    entry = {
        'type': ptype, 'has_class': has_class, 'has_trailer': trailer,
        'tsi': tsi, 'tsf': tsf, 'pkt_count': pkt_count,
        'size_words': size_words,
        'stream_id': STREAM_ID if has_sid else 0,
        'oui': OUI if has_class else 0,
        'info_class': INFO_CLASS,
        'packet_class': PACKET_CLASS if has_class else 0,
        'tsi_secs': TSI_SECS if tsi else 0,
        'tsf_frac': tsf_frac if tsf else 0,
        'prologue_bytes': prologue_bytes, 'payload_bytes': len(payload),
        'valid': True,
    }
    return data, entry


def vectors():
    out = []

    def add(name, data, entry, pattern):
        entry = dict(entry)
        entry['payload_pattern'] = pattern
        out.append((name, data, entry))

    d, e = build(1, 0, ramp16(SAMPLES), True, 1, 1, 0)
    add('if_data_sid_full.bin', d, e, 'ramp16')
    d, e = build(0, 0, ramp16(SAMPLES))
    add('if_data_nosid.bin', d, e, 'ramp16')
    d, e = build(1, 0, ramp16(SAMPLES), trailer=True)
    add('if_data_trailer.bin', d, e, 'ramp16')
    for i in range(18):
        d, e = build(1, i % 16, ramp16(SAMPLES), True, 1, 1, SAMPLES * i)
        add('if_data_wrap_seq_%02d.bin' % i, d, e, 'ramp16')
    # 8968 bytes on the wire: 28 byte prologue + 8940 byte payload.
    d, e = build(1, 0, ramp16(2235), True, 1, 1, 0)
    assert len(d) == 8968
    add('if_data_jumbo_8968.bin', d, e, 'ramp16')
    # Context packet: CIF0 word (0) plus one zero indicator-field word.
    d, e = build(4, 0, bytes(8), True, 1, 1, 0)
    add('context_cif0.bin', d, e, 'zero')
    # Header claims 100 words, only 40 bytes present.
    d, e = build(1, 0, bytes(12), True, 1, 1, 0, size_override=100)
    assert len(d) == 40
    e.update(valid=False, size_words=100)
    add('truncated_header.bin', d, e, 'zero')
    e = dict(e, size_words=0, prologue_bytes=0, payload_bytes=0)
    add('short_3bytes.bin', b'\x10\x00\x00', e, 'zero')
    return out


FIELDS = ['type', 'has_class', 'has_trailer', 'tsi', 'tsf', 'pkt_count',
          'size_words', 'stream_id', 'oui', 'info_class', 'packet_class',
          'tsi_secs', 'tsf_frac', 'prologue_bytes', 'payload_bytes', 'valid']


def c_val(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    return '0x%XULL' % v if v > 0xFFFFFFFF else '%d' % v


def main(argv):
    args = argv[1:]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    outdir = args[0]
    os.makedirs(outdir, exist_ok=True)
    vecs = vectors()
    files = []
    rows = []
    for name, data, entry in vecs:
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        files.append(dict(name=name, **entry))
        rows.append('\t{"%s", %s},' % (
            name, ', '.join(c_val(entry[k]) for k in FIELDS)))
    with open(os.path.join(outdir, 'manifest.json'), 'w') as f:
        json.dump({'files': files}, f, indent=2)
        f.write('\n')
    with open(os.path.join(outdir, 'expected.h'), 'w') as f:
        f.write('/* Generated by tests/daqiri/gen_golden.py; '
                'do not edit. */\n')
        f.write('static const struct dq_vrt_golden golden[] = {\n')
        f.write('\n'.join(rows) + '\n};\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
