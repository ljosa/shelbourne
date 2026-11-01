#!/usr/bin/env python3
"""
Stream a test tone to the BCO's own AirPlay receiver (AirTunes on port 1024).

The BCO advertises et=0 (no encryption) and cn=0,1, so this sends an
unencrypted AirPlay 1 session with uncompressed ALAC frames.  It answers the
receiver's timing requests and sends sync packets on the control port.

Usage: test_bco_airplay.py HOST [PORT] [SECONDS] [FREQ] [VOLUME_DB] [AMPLITUDE]
"""

import math
import random
import socket
import struct
import sys
import threading
import time

SAMPLE_RATE = 44100
FRAMES_PER_PACKET = 352
LATENCY = 88200          # frames between "now" and play time in sync packets
NTP_EPOCH = 2208988800   # seconds from 1900 to 1970


def ntp_now():
    t = time.time() + NTP_EPOCH
    sec = int(t)
    frac = int((t - sec) * (1 << 32)) & 0xFFFFFFFF
    return (sec << 32) | frac


def local_ip_for(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((host, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip


class Rtsp:
    def __init__(self, host, port):
        self.host, self.port = host, port
        self.sock = socket.create_connection((host, port), timeout=30)
        self.cseq = 0
        self.session = None
        self.base = {
            'User-Agent': 'iTunes/7.6.2 (Macintosh; N; PPC)',
            'Client-Instance': '%016X' % random.getrandbits(64),
            'DACP-ID': '%016X' % random.getrandbits(64),
            'Active-Remote': str(random.getrandbits(31)),
        }

    def request(self, method, url, body=b'', headers=None):
        self.cseq += 1
        h = dict(self.base)
        h['CSeq'] = str(self.cseq)
        if self.session:
            h['Session'] = self.session
        if headers:
            h.update(headers)
        if body:
            h['Content-Length'] = str(len(body))
        msg = '%s %s RTSP/1.0\r\n' % (method, url)
        msg += ''.join('%s: %s\r\n' % kv for kv in h.items()) + '\r\n'
        self.sock.sendall(msg.encode() + body)
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        head = data.split(b'\r\n\r\n', 1)[0].decode(errors='replace')
        status = head.split('\r\n', 1)[0]
        hdrs = {}
        for line in head.split('\r\n')[1:]:
            if ':' in line:
                k, v = line.split(':', 1)
                hdrs[k.strip().lower()] = v.strip()
        if 'session' in hdrs:
            self.session = hdrs['session'].split(';')[0]
        print('%-13s -> %s' % (method, status))
        return status, hdrs


def alac_uncompressed(pcm_be):
    """Wrap 16-bit big-endian stereo PCM in an uncompressed ALAC frame."""
    bits, nbits = 0, 0

    def push(val, n):
        nonlocal bits, nbits
        bits = (bits << n) | (val & ((1 << n) - 1))
        nbits += n

    push(1, 3)    # channels - 1 (stereo)
    push(0, 4)    # element instance tag
    push(0, 12)   # unused
    push(0, 1)    # hassize
    push(0, 2)    # uncompressed_bytes
    push(1, 1)    # isnotcompressed
    for i in range(0, len(pcm_be), 2):
        push((pcm_be[i] << 8) | pcm_be[i + 1], 16)
    push(7, 3)    # end-of-frame tag
    if nbits % 8:
        push(0, 8 - nbits % 8)
    return (bits).to_bytes(nbits // 8, 'big')


stats = {'timing_in': 0, 'timing_req': 0, 'control_in': 0}


def control_listener(sock, stop):
    """Count packets the receiver sends to our control port (e.g. resend requests)."""
    sock.settimeout(0.5)
    while not stop.is_set():
        try:
            sock.recvfrom(256)
        except socket.timeout:
            continue
        except OSError:
            break
        stats['control_in'] += 1


def timing_responder(sock, stop):
    """Answer the receiver's NTP-style timing requests (type 0x52)."""
    sock.settimeout(0.5)
    while not stop.is_set():
        try:
            data, addr = sock.recvfrom(128)
        except socket.timeout:
            continue
        stats['timing_in'] += 1
        if len(data) >= 32 and (data[1] & 0x7F) == 0x52:
            stats['timing_req'] += 1
            origin = data[24:32]
            now = ntp_now()
            reply = struct.pack('>BBHI', 0x80, 0xD3, 0x0007, 0) + origin + \
                struct.pack('>QQ', now, now)
            sock.sendto(reply, addr)


def main():
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 10
    freq = float(sys.argv[4]) if len(sys.argv) > 4 else 440.0
    volume_db = float(sys.argv[5]) if len(sys.argv) > 5 else -15.0
    amplitude = int(sys.argv[6]) if len(sys.argv) > 6 else 12000

    me = local_ip_for(host)
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(('', 0))
    timing = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    timing.bind(('', 0))
    audio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    stop = threading.Event()
    threading.Thread(target=timing_responder, args=(timing, stop), daemon=True).start()
    threading.Thread(target=control_listener, args=(control, stop), daemon=True).start()

    r = Rtsp(host, port)
    url = 'rtsp://%s/%d' % (me, random.getrandbits(31))
    r.request('OPTIONS', '*')
    sdp = ('v=0\r\n'
           'o=iTunes %d 0 IN IP4 %s\r\n'
           's=iTunes\r\n'
           'c=IN IP4 %s\r\n'
           't=0 0\r\n'
           'm=audio 0 RTP/AVP 96\r\n'
           'a=rtpmap:96 AppleLossless\r\n'
           'a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %d\r\n'
           % (random.getrandbits(31), me, host, FRAMES_PER_PACKET, SAMPLE_RATE))
    status, _ = r.request('ANNOUNCE', url, sdp.encode(),
                          {'Content-Type': 'application/sdp'})
    if ' 200 ' not in status + ' ':
        print('ANNOUNCE rejected')
        return 1
    transport = ('RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;'
                 'control_port=%d;timing_port=%d'
                 % (control.getsockname()[1], timing.getsockname()[1]))
    status, hdrs = r.request('SETUP', url, headers={'Transport': transport})
    params = dict(p.split('=', 1) for p in hdrs.get('transport', '').split(';') if '=' in p)
    server_port = int(params.get('server_port', 0))
    server_control = int(params.get('control_port', 0))
    print('server_port=%d control_port=%d timing_port=%s'
          % (server_port, server_control, params.get('timing_port')))
    if not server_port:
        return 1

    seq = random.getrandbits(16)
    rtptime = random.getrandbits(32)
    r.request('RECORD', url, headers={'Range': 'npt=0-',
                                      'RTP-Info': 'seq=%d;rtptime=%d' % (seq, rtptime)})
    r.request('SET_PARAMETER', url, ('volume: %f\r\n' % volume_db).encode(),
              {'Content-Type': 'text/parameters'})

    ssrc = random.getrandbits(32)
    n_packets = int(seconds * SAMPLE_RATE / FRAMES_PER_PACKET)
    phase = 0
    start = time.monotonic()
    last_sync = -1
    print('streaming %.1f s of %.0f Hz to %s:%d' % (seconds, freq, host, server_port))
    for i in range(n_packets):
        if server_control and int((time.monotonic() - start)) != last_sync:
            last_sync = int(time.monotonic() - start)
            sync = struct.pack('>BBHIQI', 0x90 if i == 0 else 0x80, 0xD4, 0x0007,
                               (rtptime - LATENCY) & 0xFFFFFFFF, ntp_now(), rtptime)
            control.sendto(sync, (host, server_control))
        pcm = bytearray()
        for _ in range(FRAMES_PER_PACKET):
            v = int(amplitude * math.sin(phase))
            phase += 2 * math.pi * freq / SAMPLE_RATE
            pcm += struct.pack('>hh', v, v)
        header = struct.pack('>BBHII', 0x80, 0xE0 if i == 0 else 0x60,
                             seq & 0xFFFF, rtptime & 0xFFFFFFFF, ssrc)
        audio.sendto(header + alac_uncompressed(bytes(pcm)), (host, server_port))
        seq += 1
        rtptime += FRAMES_PER_PACKET
        target = start + (i + 1) * FRAMES_PER_PACKET / SAMPLE_RATE
        delay = target - time.monotonic()
        if delay > 0:
            time.sleep(delay)

    time.sleep(2.5)   # let the receiver play out its buffer
    r.request('TEARDOWN', url)
    stop.set()
    print('receiver packets: timing=%d (requests %d), control=%d'
          % (stats['timing_in'], stats['timing_req'], stats['control_in']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
