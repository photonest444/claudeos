#!/usr/bin/env python3
"""Headless smoke test: boots the ISO in QEMU, drives the CLI through the
serial port and checks the output. Usage: smoketest.py build/claudeos.iso"""
import re, subprocess, sys, threading, time

CHECKS = [   # (command, text expected in the output)
    ('ls /bin', 'hello'),
    ('hello a b', 'argv[2] = "b"'),
    ('echo test123 > /t.txt', ''),
    ('cat /t.txt', 'test123'),
    ('mkdir /d1', ''),
    ('ls /', 'd1/'),
    ('rm /d1', ''),
    ('primes 50', '15 primes <= 50'),
    ('wc /docs/readme.txt', 'readme.txt'),
    ('crash', 'process killed'),
    ('ps', 'display'),
    ('mem', 'Physical memory'),
]

def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else 'build/claudeos.iso'
    q = subprocess.Popen(['qemu-system-i386', '-cdrom', iso, '-m', '128M', '-serial', 'stdio',
                          '-display', 'none', '-no-reboot'],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray()
    threading.Thread(target=lambda: [buf.extend(c) for c in iter(lambda: q.stdout.read(1), b'')],
                     daemon=True).start()
    def text():
        return re.sub(r'\x1b\[[0-9;]*m', '', buf.decode('utf-8', 'replace'))
    deadline = time.time() + 30
    while 'root@claudeos' not in text():
        if time.time() > deadline:
            print(text()); sys.exit('FAIL: no shell prompt within 30 s')
        time.sleep(0.2)
    failed = 0
    for cmd, expect in CHECKS:
        start = len(text())
        q.stdin.write((cmd + '\r').encode()); q.stdin.flush()
        t0 = time.time()
        def done():     # expected text seen, or (no expectation) the next prompt
            out = text()[start:]
            return expect in out if expect else 'root@claudeos' in out
        while time.time() - t0 < 5 and not done():
            time.sleep(0.1)
        time.sleep(0.3)
        ok = expect in text()[start:]
        failed += not ok
        print('%-4s %s' % ('ok' if ok else 'FAIL', cmd))
    q.stdin.write(b'shutdown\r'); q.stdin.flush()
    try:
        q.wait(10)
        print('ok   shutdown (QEMU powered off)')
    except subprocess.TimeoutExpired:
        q.kill(); failed += 1; print('FAIL shutdown')
    if failed:
        print(text()); sys.exit('%d check(s) failed' % failed)
    print('All checks passed.')

if __name__ == '__main__':
    main()
