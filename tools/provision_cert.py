#!/usr/bin/env python3
"""
Simple provisioning helper: convert a PEM blob into a partition image suitable for
flashing into the `esp_secure_cert` data partition defined in `partitions.csv`.

Usage:
    python tools/provision_cert.py --ca ca.pem --cert device.crt --key device.key --out esp_secure_cert.bin --size 4096

Then flash with (example, Windows PowerShell):
  esptool.py --chip esp32 --port COM3 write_flash 0x10000 esp_secure_cert.bin

Adjust the offset to the partition offset in `partitions.csv` before flashing.

Note: flashing device secrets must be done in a secure environment.
"""
import argparse
import os

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--ca', required=True)
    p.add_argument('--cert', required=True)
    p.add_argument('--key', required=False)
    p.add_argument('--out', default='esp_secure_cert.bin')
    p.add_argument('--size', type=int, default=0x4000)
    args = p.parse_args()

    # Create TLV partition image. Format:
    # 4 bytes magic 'SPCF', 1 byte version, then TLVs: [1-byte type][4-byte little-endian length][data]
    def read_optional(path):
        if not path:
            return None
        with open(path, 'rb') as f:
            return f.read()

    ca = read_optional(args.ca)
    cert = read_optional(args.cert)
    key = read_optional(args.key)

    buf = bytearray()
    buf += b'SPCF'  # magic
    buf += bytes([1])  # version

    def append_tlv(t, data):
        if not data: return
        buf.append(t)
        l = len(data)
        buf += (l).to_bytes(4, 'little')
        buf += data

    # types: 1 = CA list, 2 = client cert, 3 = client key
    append_tlv(1, ca)
    append_tlv(2, cert)
    append_tlv(3, key)

    if len(buf) > args.size:
        raise SystemExit('TLV image larger than partition size')
    # pad with 0xFF
    buf += b'\xFF' * (args.size - len(buf))
    with open(args.out, 'wb') as f:
        f.write(buf)
    print('Wrote', args.out, 'size=', len(buf))
    print('Flash with esptool.py write_flash at the partition offset from partitions.csv')

if __name__ == '__main__':
    main()
