#!/usr/bin/env python3
"""Send a firmware image to the S-Link bridge over WiFi (push update).

    python tools/ota_push.py slink.local .pio/build/esp32s3/firmware.bin
    python tools/ota_push.py 192.168.178.147 build/esp32c3_slink.bin --password secret
    python tools/ota_push.py slink.local            # no file: --build ENV runs PlatformIO and sends its image

The password is the one set in the portal (Settings > Firmware update) or with the CLI command "OTA PASS <text>". It is
optional: the device asks for it only when one is set. It never goes on the network: the device gives a one-time nonce
(GET /api/ota) and the tool answers with sha256("<nonce>:<password>"). The password can also come from the SLINK_OTA_PASSWORD
environment variable. Only the standard library is used.
"""
import argparse, hashlib, http.client, json, os, subprocess, sys, time


def die(msg, code=1):
    print('ERROR: ' + msg, file=sys.stderr)
    sys.exit(code)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help='address or name of the device (slink.local, 192.168.1.50, host:port)')
    ap.add_argument('image', nargs='?', help='application image (.bin); default: the PlatformIO build selected with --build')
    ap.add_argument('-p', '--password', default=os.environ.get('SLINK_OTA_PASSWORD'), help='upload password, if one is set on the device')
    ap.add_argument('--force', action='store_true', help='install even if the device already runs this very build')
    ap.add_argument('--build', metavar='ENV', help='build this PlatformIO environment first (esp32c3, esp32c6, esp32s3) and send its image')
    ap.add_argument('--timeout', type=float, default=60, help='network timeout in seconds (default 60)')
    a = ap.parse_args()

    image = a.image
    if a.build:
        print('building %s ...' % a.build)
        if subprocess.call(['platformio', 'run', '-e', a.build]) != 0:
            die('build failed')
        image = os.path.join('.pio', 'build', a.build, 'firmware.bin')
    if not image:
        die('give the image file (.bin) or --build ENV')
    try:
        data = open(image, 'rb').read()
    except OSError as e:
        die('cannot read %s: %s' % (image, e))

    host, _, port = a.host.partition(':')
    conn = http.client.HTTPConnection(host, int(port) if port else 80, timeout=a.timeout)
    try:
        conn.request('GET', '/api/ota')
        r = conn.getresponse()
        body = r.read()
        if r.status != 200:
            die('the device answered %d to GET /api/ota: is the firmware an OTA-capable build?' % r.status)
        info = json.loads(body)
    except (OSError, ValueError) as e:
        die('cannot reach %s: %s' % (a.host, e))
    print('device: %s, firmware %s, running from %s (%s); update slot %d bytes' % (info['target'], info['fw'], info['running'], info['state'], info['max']))
    if info.get('push') is False:
        die('uploads are disabled on the device: enable them first (Settings > Firmware update > "Consenti l\'invio dall\'esterno", or the CLI command OTA PUSH ON)')
    if len(data) > info['max']:
        die('the image (%d bytes) does not fit the update slot (%d bytes)' % (len(data), info['max']))
    if info.get('busy'):
        die('the device is already updating')

    headers = {'Content-Type': 'application/octet-stream', 'Content-Length': str(len(data))}
    headers['X-OTA-Time'] = str(int(time.time()))      # the device has no clock: it records this as the installation date
    if a.force:
        headers['X-OTA-Force'] = '1'
    if info.get('auth'):
        if not a.password:
            die('the device asks for a password: use --password or SLINK_OTA_PASSWORD')
        headers['X-OTA-Nonce'] = info['nonce']
        headers['X-OTA-Auth'] = hashlib.sha256(('%s:%s' % (info['nonce'], a.password)).encode()).hexdigest()

    print('sending %s (%d bytes) ...' % (image, len(data)))
    t0 = time.time()
    try:
        conn.putrequest('POST', '/api/ota')
        for k, v in headers.items():
            conn.putheader(k, v)
        conn.endheaders()
        sent = 0
        step = 16384
        while sent < len(data):
            conn.send(data[sent:sent + step])
            sent += step
            sent = min(sent, len(data))
            print('\r  %3d%%' % (sent * 100 // len(data)), end='', flush=True)
        print()
        r = conn.getresponse()
        msg = r.read().decode(errors='replace')
    except (OSError, http.client.HTTPException) as e:
        die('transfer failed: %s (wrong password? the device may have closed the connection)' % e)
    if r.status != 200:
        die('rejected by the device (%d): %s' % (r.status, msg))
    print('%s  (%.1f s)' % (msg, time.time() - t0))
    print('The device restarts into the new firmware; it confirms itself after ~30 s, otherwise it goes back to the previous one.')


if __name__ == '__main__':
    main()
