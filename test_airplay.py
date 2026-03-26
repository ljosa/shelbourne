#!/usr/bin/env python3
"""
Full AirPlay 1 streaming test: handshake → announce → setup → stream audio.
Sends a 440Hz sine wave to the AirPlay receiver and verifies audio flows.
"""

import socket, os, struct, hashlib, time, math, base64
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import rsa, padding as rsa_padding
from cryptography.hazmat.primitives.serialization import load_pem_private_key

HOST = '192.168.2.42'
SAMPLE_RATE = 44100
CHANNELS = 2
BITS = 16
FRAMES_PER_PACKET = 352  # standard ALAC packet size for AirPlay

# Well-known AirPort Express RSA private key (to extract public key)
AIRPORT_KEY_PEM = b"""-----BEGIN RSA PRIVATE KEY-----
MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt
wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U
wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf
/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/
UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW
BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa
LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5
NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm
lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz
aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu
a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM
oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z
oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+
k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL
AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA
cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf
54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov
17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc
1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI
LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ
2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=
-----END RSA PRIVATE KEY-----"""


def send_recv(s, method, url, cseq, data=None, content_type=None, extra_headers=None):
    req = f'{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n'
    if extra_headers:
        for k, v in extra_headers.items():
            req += f'{k}: {v}\r\n'
    if data is not None:
        ct = content_type or 'application/octet-stream'
        if isinstance(data, str):
            data = data.encode()
        req += f'Content-Type: {ct}\r\nContent-Length: {len(data)}\r\n'
    else:
        data = b''
    req += '\r\n'
    s.sendall(req.encode() + data)
    resp = b''
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
            if b'\r\n\r\n' in resp:
                hdr_bytes, body = resp.split(b'\r\n\r\n', 1)
                hdr = hdr_bytes.decode()
                for line in hdr.split('\r\n'):
                    if line.lower().startswith('content-length:'):
                        exp = int(line.split(':')[1].strip())
                        if len(body) >= exp:
                            return hdr, body[:exp]
                return hdr, body
        except socket.timeout:
            break
    if resp and b'\r\n\r\n' in resp:
        h, b = resp.split(b'\r\n\r\n', 1)
        return h.decode(), b
    return '', b''


def derive_key(shared_secret, salt):
    return hashlib.sha512(salt + shared_secret).digest()[:16]


def do_handshake(s):
    """Complete pair-setup + pair-verify + fp-setup."""
    send_recv(s, 'GET', '/info', 0)

    client_ed = Ed25519PrivateKey.generate()
    client_ed_pub = client_ed.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    _, srv_pk = send_recv(s, 'POST', '/pair-setup', 1, data=client_ed_pub)
    assert len(srv_pk) == 32, f'pair-setup failed: {len(srv_pk)}'

    client_x = X25519PrivateKey.generate()
    client_x_pub = client_x.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    _, pv1 = send_recv(s, 'POST', '/pair-verify', 2,
                       data=bytes([1, 0, 0, 0]) + client_x_pub + client_ed_pub)
    assert len(pv1) == 96, f'pair-verify p1 failed: {len(pv1)}'

    ss = client_x.exchange(X25519PublicKey.from_public_bytes(pv1[:32]))
    k = derive_key(ss, b"Pair-Verify-AES-Key")
    iv = derive_key(ss, b"Pair-Verify-AES-IV")
    d = Cipher(algorithms.AES(k), modes.CTR(iv)).decryptor()
    d.update(pv1[32:96])
    enc_sig = d.update(client_ed.sign(client_x_pub + pv1[:32]))
    h, _ = send_recv(s, 'POST', '/pair-verify', 3,
                     data=bytes([0, 0, 0, 0]) + enc_sig)
    assert '200' in h and 'close' not in h.lower(), f'pair-verify p2 failed'

    fp1 = bytes([0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00,
                 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00])
    _, r1 = send_recv(s, 'POST', '/fp-setup', 5, data=fp1)
    assert len(r1) == 142
    fp2 = (bytes([0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x03, 0x00,
                  0x00, 0x00, 0x00, 0xa0]) + os.urandom(152))
    _, r2 = send_recv(s, 'POST', '/fp-setup', 6, data=fp2)
    assert len(r2) == 32
    print('Handshake + FairPlay: OK')


def do_announce_setup(s, aes_key, aes_iv):
    """ANNOUNCE with RSA-encrypted AES key, then SETUP."""
    # Load the airport RSA key to get the public key for encryption
    rsa_privkey = load_pem_private_key(AIRPORT_KEY_PEM, password=None)
    rsa_pubkey = rsa_privkey.public_key()

    # RSA-OAEP encrypt the AES session key
    encrypted_aes_key = rsa_pubkey.encrypt(
        aes_key,
        rsa_padding.OAEP(
            mgf=rsa_padding.MGF1(algorithm=hashes.SHA1()),
            algorithm=hashes.SHA1(),
            label=None
        )
    )

    rsaaeskey_b64 = base64.b64encode(encrypted_aes_key).decode()
    aesiv_b64 = base64.b64encode(aes_iv).decode()

    sdp = (
        "v=0\r\n"
        "o=iTunes 1 0 IN IP4 192.168.2.49\r\n"
        "s=iTunes\r\n"
        "c=IN IP4 192.168.2.49\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        f"a=fmtp:96 {FRAMES_PER_PACKET} 0 {BITS} 40 10 14 {CHANNELS} 255 0 0 {SAMPLE_RATE}\r\n"
        f"a=rsaaeskey:{rsaaeskey_b64}\r\n"
        f"a=aesiv:{aesiv_b64}\r\n"
    )

    hdr, _ = send_recv(s, 'ANNOUNCE', '/stream', 7,
                       data=sdp, content_type='application/sdp')
    assert '200' in hdr, f'ANNOUNCE failed: {hdr}'
    print('ANNOUNCE: OK (RSA-encrypted AES key)')

    # SETUP: request UDP transport
    transport = ("RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
                 "control_port=6001;timing_port=6002")
    hdr, _ = send_recv(s, 'SETUP', '/stream', 8,
                       extra_headers={'Transport': transport})
    assert '200' in hdr, f'SETUP failed: {hdr}'

    # Parse server's transport response to get ports
    server_port = None
    for line in hdr.split('\r\n'):
        if line.startswith('Transport:'):
            for part in line.split(';'):
                if part.strip().startswith('server_port='):
                    server_port = int(part.split('=')[1])
            print(f'SETUP: OK (server_port={server_port})')
    return server_port


def generate_sine_pcm(freq, duration_sec, sample_rate, channels, bits):
    """Generate raw PCM sine wave."""
    num_samples = int(sample_rate * duration_sec)
    samples = []
    max_val = (1 << (bits - 1)) - 1
    for i in range(num_samples):
        val = int(max_val * 0.5 * math.sin(2 * math.pi * freq * i / sample_rate))
        for _ in range(channels):
            samples.append(struct.pack('<h', val))
    return b''.join(samples)


def build_alac_frame(samples_be):
    """Wrap big-endian 16-bit stereo PCM samples in an uncompressed ALAC frame.

    ALAC uncompressed stereo 16-bit header (23 bits):
      channels=1 (3 bits), element_tag=0 (4 bits), unused=0 (12 bits),
      hassize=0 (1 bit), uncompressed_bytes=0 (2 bits), isnotcompressed=1 (1 bit)
    Then 352 pairs of 16-bit big-endian samples (L, R).
    Total: 23 bits header + 352*32 bits = 11287 bits, padded to 1411 bytes.
    """
    # Build the 23-bit header as a bit stream
    # channels=1 (3 bits): 001
    # element_tag=0 (4 bits): 0000
    # unused=0 (12 bits): 000000000000
    # hassize=0 (1 bit): 0
    # uncompressed_bytes=0 (2 bits): 00
    # isnotcompressed=1 (1 bit): 1
    # Total: 0010000 00000000 0000001x = 0x200001 in 23 bits
    # As bytes: [0x20, 0x00, 0x01] but shifted since it's 23 bits (not byte-aligned)
    # Bit layout: 001 0000 000000000000 0 00 1 = 23 bits
    # Byte 0: 0010_0000 = 0x20
    # Byte 1: 0000_0000 = 0x00
    # Byte 2: 001_xxxxx = first bit is the isnotcompressed=1, rest is audio data
    # Actually, let's construct it properly with bit packing

    bits = 0
    nbits = 0

    def push(val, n):
        nonlocal bits, nbits
        bits = (bits << n) | (val & ((1 << n) - 1))
        nbits += n

    push(1, 3)   # channels = 1 (stereo)
    push(0, 4)   # element instance tag
    push(0, 12)  # unused
    push(0, 1)   # hassize = 0
    push(0, 2)   # uncompressed_bytes = 0
    push(1, 1)   # isnotcompressed = 1 (uncompressed)
    # 23 bits so far

    # Now append the audio samples (each is 16-bit big-endian, L then R)
    for i in range(0, len(samples_be), 2):
        val = (samples_be[i] << 8) | samples_be[i+1]
        push(val, 16)

    # Pad to byte boundary
    if nbits % 8 != 0:
        push(0, 8 - (nbits % 8))

    # Convert to bytes
    total_bytes = nbits // 8
    result = bytearray(total_bytes)
    for i in range(total_bytes - 1, -1, -1):
        result[i] = bits & 0xFF
        bits >>= 8

    return bytes(result)


def build_alac_rtp_packet(seq, timestamp, ssrc, alac_frame, aes_key, aes_iv):
    """Build an RTP packet with AES-CBC encrypted ALAC audio."""
    # RTP header (12 bytes)
    # V=2, P=0, X=0, CC=0, M=1, PT=96
    rtp_header = struct.pack('>BBHII',
                             0x80,       # V=2, P=0, X=0, CC=0
                             0x60 | 96,  # M=1, PT=96
                             seq & 0xFFFF,
                             timestamp,
                             ssrc)

    # AES-CBC encrypt in 16-byte blocks, leave remainder unencrypted
    encrypted = bytearray()
    block_size = 16
    num_blocks = len(alac_frame) // block_size
    remainder = len(alac_frame) % block_size

    if num_blocks > 0:
        cipher = Cipher(algorithms.AES(aes_key), modes.CBC(aes_iv))
        enc = cipher.encryptor()
        encrypted.extend(enc.update(alac_frame[:num_blocks * block_size]))
        # Don't finalize - no padding in AirPlay

    if remainder > 0:
        encrypted.extend(alac_frame[num_blocks * block_size:])

    return rtp_header + bytes(encrypted)


def main():
    import subprocess

    # Find port via mDNS
    proc = subprocess.Popen(['dns-sd', '-L', 'Mr Noisy', '_airplay._tcp'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(3)
    proc.kill()
    output = proc.stdout.read().decode()
    port = None
    for line in output.split('\n'):
        if 'can be reached at' in line:
            for p in line.split(':'):
                p = p.strip().split()[0] if p.strip() else ''
                try:
                    v = int(p)
                    if v > 1024:
                        port = v
                        break
                except:
                    continue
    if not port:
        print(f'Could not find Mr Noisy via mDNS')
        return
    print(f'Found Mr Noisy at {HOST}:{port}')

    # Connect
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, port))

    # Handshake
    do_handshake(s)

    # Generate AES session key and IV
    aes_key = os.urandom(16)
    aes_iv = os.urandom(16)

    # ANNOUNCE + SETUP
    server_port = do_announce_setup(s, aes_key, aes_iv)
    if not server_port:
        print('No server port received, cannot stream')
        s.close()
        return

    # RECORD
    hdr, _ = send_recv(s, 'RECORD', '/stream', 9,
                       extra_headers={
                           'Range': 'npt=0-',
                           'RTP-Info': 'seq=0;rtptime=0'
                       })
    print(f'RECORD: {"OK" if "200" in hdr else "FAIL"}')

    # Stream audio: 440Hz sine wave for 5 seconds
    print(f'Streaming 440Hz sine wave for 5 seconds to port {server_port}...')

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ssrc = 0xDEADBEEF
    seq = 0
    timestamp = 0
    bytes_per_frame = CHANNELS * (BITS // 8)  # 4 bytes per frame
    packet_bytes = FRAMES_PER_PACKET * bytes_per_frame  # 1408 bytes per packet
    packet_duration = FRAMES_PER_PACKET / SAMPLE_RATE  # ~7.98ms

    # Generate 5 seconds of sine wave
    total_packets = int(5.0 / packet_duration)
    start_time = time.time()

    for i in range(total_packets):
        # Generate one packet worth of big-endian PCM samples (L, R interleaved)
        samples_be = bytearray()
        for j in range(FRAMES_PER_PACKET):
            sample_idx = i * FRAMES_PER_PACKET + j
            val = int(16000 * math.sin(2 * math.pi * 440 * sample_idx / SAMPLE_RATE))
            samples_be.extend(struct.pack('>hh', val, val))  # L, R big-endian

        # Wrap in ALAC uncompressed frame, then encrypt
        alac_frame = build_alac_frame(samples_be)
        pkt = build_alac_rtp_packet(seq, timestamp, ssrc, alac_frame, aes_key, aes_iv)
        udp.sendto(pkt, (HOST, server_port))

        seq += 1
        timestamp += FRAMES_PER_PACKET

        # Pace to real-time
        expected = start_time + (i + 1) * packet_duration
        now = time.time()
        if expected > now:
            time.sleep(expected - now)

    elapsed = time.time() - start_time
    print(f'Sent {total_packets} packets in {elapsed:.1f}s')

    # TEARDOWN
    hdr, _ = send_recv(s, 'TEARDOWN', '/stream', 10)
    print(f'TEARDOWN: {"OK" if "200" in hdr else hdr[:100]}')

    udp.close()
    s.close()
    print('Done.')


if __name__ == '__main__':
    main()
