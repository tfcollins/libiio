#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the VITA 49.2 golden vectors used by the DAQIRI backend tests.

Usage: gen_golden.py <outdir>

Writes the *.bin vectors, manifest.json and expected.h (the C table the
parser test compiles in) into <outdir>. Output is deterministic. Run
./format.sh after regenerating tests/resources/vrt so expected.h is
clang-format clean.

Payload pattern "ramp16": sample n has I = n & 0x7FFF and
Q = (0x8000 - n) & 0xFFFF, each packed as a big-endian 16-bit word, I first.
Payload pattern "zero": all zero bytes.

The l2-*.bin vectors are Ethernet frames for the raw-queue parser
(daqiri-l2.c): Ethernet II, optional 802.1Q, IPv4 (checksummed), UDP
(checksummed), then a VRT packet. Their manifest entries have "kind": "l2"
and the expected dq_l2_parse() status and info fields; expected.h carries
them in l2_golden[]; a test defining DQ_L2_GOLDEN before including it
sees only that table (struct dq_l2_golden), others only golden[].

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
PACKET_CLASS = 0x0010  # vrt49 CI16 (vrt49 reference/packet-classes.md)
TSI_SECS = 1700000000
SAMPLES = 360
TRAILER = 0x00000000

L2_ETH_DST = bytes([0x02, 0, 0, 0, 0, 0x02])
L2_ETH_SRC = bytes([0x02, 0, 0, 0, 0, 0x01])
L2_IP_SRC = 0x0A000001  # 10.0.0.1
L2_IP_DST = 0x0A000002  # 10.0.0.2
L2_UDP_SRC = 49152
L2_UDP_DST = 25101
L2_VLAN_ID = 100


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


def inet_csum(data):
    """RFC 1071 ones-complement sum of 16-bit big-endian words."""
    if len(data) % 2:
        data += b'\x00'
    total = sum(struct.unpack('>%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def build_l2(payload, *, vlan=None, qinq=False, proto=17, ihl=5, frag=0,
             ip_len=None, udp_len=None, truncate=None, ethertype=None):
    """Return (bytes, dict) for one Ethernet/[802.1Q]/IPv4/UDP frame.

    vlan: VLAN ID of a single 802.1Q tag. qinq: outer 802.1ad tag (ID 200)
    plus inner 802.1Q tag (vlan, default L2_VLAN_ID). frag: the IPv4
    flags/fragment-offset field. ip_len / udp_len override the lengths
    written into the headers; truncate cuts the frame to that many bytes
    after the checksums are computed; ethertype overrides the final
    EtherType (the IPv4 header is still built).
    """
    udp_total = 8 + len(payload) if udp_len is None else udp_len
    real_udp = 8 + len(payload)
    udp = struct.pack('>HHHH', L2_UDP_SRC, L2_UDP_DST, udp_total, 0)
    udp += payload
    pseudo = struct.pack('>IIBBH', L2_IP_SRC, L2_IP_DST, 0, proto,
                         udp_total)
    csum = inet_csum(pseudo + udp) or 0xFFFF
    udp = udp[:6] + struct.pack('>H', csum) + udp[8:]
    ip_total = ihl * 4 + real_udp if ip_len is None else ip_len
    ip = struct.pack('>BBHHHBBHII', (4 << 4) | ihl, 0, ip_total, 0x1234,
                     frag, 64, proto, 0, L2_IP_SRC, L2_IP_DST)
    ip += bytes(4 * (ihl - 5))  # zero options
    ip = ip[:10] + struct.pack('>H', inet_csum(ip)) + ip[12:]
    eth = L2_ETH_DST + L2_ETH_SRC
    if qinq:
        eth += struct.pack('>HHHH', 0x88A8, 200, 0x8100,
                           L2_VLAN_ID if vlan is None else vlan)
    elif vlan is not None:
        eth += struct.pack('>HH', 0x8100, vlan)
    eth += struct.pack('>H', 0x0800 if ethertype is None else ethertype)
    data = eth + ip + udp
    if truncate is not None:
        data = data[:truncate]
    off = len(eth) + ihl * 4 + 8
    entry = {
        'payload_off': len(eth) + 20 + 8 if ihl == 5 else off,
        'payload_len': len(payload),
        'udp_src': L2_UDP_SRC, 'udp_dst': L2_UDP_DST,
        'vlan_id': 0 if vlan is None or qinq else vlan,
        'ip_src': L2_IP_SRC, 'ip_dst': L2_IP_DST,
    }
    return data, entry


L2_FIELDS = ['payload_off', 'payload_len', 'udp_src', 'udp_dst', 'vlan_id',
             'ip_src', 'ip_dst']


def l2_vectors():
    """Return [(name, data, manifest entry)] for the l2-*.bin vectors."""
    out = []
    vrt, _ = build(1, 0, ramp16(16), True, 1, 1, 0)  # 28 + 64 bytes
    assert len(vrt) == 92

    def add(name, status, data, entry):
        entry = dict(entry)
        if status != 'DQ_L2_OK':
            entry.update({k: 0 for k in L2_FIELDS})
        out.append((name, data, dict(kind='l2', status=status, **entry)))

    d, e = build_l2(vrt)
    add('l2-plain.bin', 'DQ_L2_OK', d, e)
    d, e = build_l2(vrt, vlan=L2_VLAN_ID)
    add('l2-vlan.bin', 'DQ_L2_OK', d, e)
    d, e = build_l2(vrt, qinq=True)
    add('l2-qinq.bin', 'DQ_L2_QINQ', d, e)
    d, e = build_l2(vrt, ethertype=0x86DD)
    add('l2-ipv6.bin', 'DQ_L2_NOT_IPV4', d, e)
    d, e = build_l2(vrt, ihl=6)
    add('l2-ihl6.bin', 'DQ_L2_BAD_IHL', d, e)
    d, e = build_l2(vrt, proto=6)
    add('l2-tcp.bin', 'DQ_L2_NOT_UDP', d, e)
    d, e = build_l2(vrt, frag=0x2000)  # More Fragments
    add('l2-frag.bin', 'DQ_L2_FRAGMENT', d, e)
    d, e = build_l2(vrt, ip_len=2000, truncate=120)
    assert len(d) == 120
    add('l2-trunc-ip.bin', 'DQ_L2_BAD_IP_LEN', d, e)
    d, e = build_l2(vrt, udp_len=2000)
    add('l2-trunc-udp.bin', 'DQ_L2_BAD_UDP_LEN', d, e)
    d, e = build_l2(vrt, truncate=41)
    assert len(d) == 41
    add('l2-short.bin', 'DQ_L2_TRUNCATED', d, e)
    return out


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
    if isinstance(v, str):
        return v
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
    l2rows = []
    for name, data, entry in l2_vectors():
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        files.append(dict(name=name, **entry))
        l2rows.append('\t{"%s", %s},' % (name, ', '.join(
            ['%s' % entry['status']] + [
                '0x%08XU' % entry[k] if k.startswith('ip_') else
                c_val(entry[k]) for k in L2_FIELDS])))
    with open(os.path.join(outdir, 'manifest.json'), 'w') as f:
        json.dump({'files': files}, f, indent=2)
        f.write('\n')
    with open(os.path.join(outdir, 'expected.h'), 'w') as f:
        f.write('/* Generated by tests/daqiri/gen_golden.py; '
                'do not edit. */\n')
        f.write('#ifndef DQ_L2_GOLDEN\n')
        f.write('static const struct dq_vrt_golden golden[] = {\n')
        f.write('\n'.join(rows) + '\n};\n')
        f.write('#else\n')
        f.write('static const struct dq_l2_golden l2_golden[] = {\n')
        f.write('\n'.join(l2rows) + '\n};\n')
        f.write('#endif\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
