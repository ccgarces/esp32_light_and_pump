#!/usr/bin/env python3
"""
Compute SHA256 key-id (hex) and short id for PEM or DER cert files.
Usage: keyid.py cert.pem
"""
import sys, hashlib

def load_pem_der(path):
    data = open(path,'rb').read()
    if b'-----BEGIN' in data:
        # extract base64 between BEGIN and END
        import re, base64
        m = re.search(b"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", data, re.S)
        if not m:
            raise SystemExit('no PEM certificate found')
        b64 = b"".join(m.group(1).split())
        return base64.b64decode(b64)
    return data

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage: keyid.py cert.pem'); sys.exit(2)
    der = load_pem_der(sys.argv[1])
    fp = hashlib.sha256(der).digest()
    full = fp.hex()
    short = full[:16]
    print('keyid:', full)
    print('short:', short)
