#!/usr/bin/env python3
"""MCP (Model Context Protocol) server for the S-Link bridge: lets an AI assistant (Claude Desktop, Claude Code, any MCP client) control and read the
Sony amplifier through the device's web portal. Only the Python standard library is used (Python 3.8+).

    python tools/mcp_slink.py                           # stdio: what Claude Desktop / Claude Code / Cursor start
    python tools/mcp_slink.py --host 192.168.1.50       # the device address (default slink.local, or $SLINK_HOST)
    python tools/mcp_slink.py --http 8765               # Streamable HTTP on 127.0.0.1:8765/mcp (MCP clients on other machines: --bind 0.0.0.0 --token SECRET)
    python tools/mcp_slink.py --dry-run                 # logs the commands instead of sending them (to try the tools)

What it gives to the assistant (MCP 2025-06-18; 2025-03-26 and 2024-11-05 are accepted too):
    tools      get_status, get_radio, get_device_info, refresh_status, power, set_volume, change_volume, mute, select_input, tune, tune_station, recall_preset, seek,
               set_sound_field, get_eq, set_eq, all_off, console_command, send_raw_frame (off unless --allow-raw), recent_log
    resources  slink://state, slink://radio/presets, slink://radio/stations, slink://radio/favorites, slink://device/stats, slink://device/help, slink://log
               (stdio: subscribe to slink://state and get notified when the amplifier changes)
    prompts    radio_evening, check_system, safe_listening

Safety: the device has no login, and a loud sound is the one thing an assistant must not cause by mistake:
    - the volume never goes above --max-volume (default 30, % of the knob travel; $SLINK_MAX_VOLUME) whatever the assistant asks; volume tools refuse while the amp is in standby
    - console_command refuses the commands that change the network, the security or the firmware (WIFI, OTA, CFG, HTTPS, HUE, BLE ..., VOL ...) unless --allow-admin
    - raw bus frames need --allow-raw
    - the HTTP mode listens on 127.0.0.1 unless --bind says otherwise, refuses foreign Origin headers, and can require a bearer token (--token or $SLINK_MCP_TOKEN)
"""
import argparse, base64, collections, hashlib, http.client, json, os, re, socket, ssl, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = '1.0'
PROTOCOLS = ['2025-06-18', '2025-03-26', '2024-11-05']
INPUTS = ['TUNER', 'PHONO', 'CD', 'MD', 'DAT', 'TAPE', 'VIDEO1', 'VIDEO2', 'VIDEO3', 'DVD', 'SAT', 'TV']
BLOCKED = {'REBOOT', 'RESTART', 'WIFI', 'WIFIPWR', 'OTA', 'CFG', 'MQTT', 'HOSTNAME', 'WEB', 'APP', 'HTTPS', 'HUE', 'BLE', 'BLENAME', 'SLINKE', 'SLINKETX', 'ECO', 'UITITLE', 'VOLCFG',
           'PAD', 'SLINK', 'LED', 'BLINK', 'LEVEL', 'STATION', 'DEV', 'REPEAT', 'GAP', 'LOOPBACK', 'DECODE', 'ECHO', 'VOL'}
HEX = re.compile(r'^([0-9A-Fa-f]{2}\s*)+$')


def log(*a):
    print('[slink-mcp]', *a, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------------------------------------------- the device
class Device:
    def __init__(self, host, tls=False, cafile=None, dry=False):
        self.host, self.tls, self.dry = host, tls, dry
        self.port = 443 if tls else 80
        self.ctx = ssl.create_default_context(cafile=cafile) if tls else None
        self.sock = None
        self.lock = threading.Lock()          # one command at a time
        self.cv = threading.Condition()
        self.seq = 0
        self.lines = collections.deque(maxlen=500)
        self.state = None
        self.listeners = []
        self.last = 0.0

    # -- REST
    def get(self, path, timeout=10):
        c = http.client.HTTPSConnection(self.host, self.port, timeout=timeout, context=self.ctx) if self.tls else http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            c.request('GET', path)
            r = c.getresponse()
            body = r.read().decode('utf-8', 'replace')
            if r.status != 200:
                raise RuntimeError('%s answered HTTP %d' % (path, r.status))
            return body
        finally:
            c.close()

    def get_json(self, path):
        return json.loads(self.get(path))

    # -- WebSocket (the commands answer there)
    def _connect(self):
        raw = socket.create_connection((self.host, self.port), timeout=8)
        s = self.ctx.wrap_socket(raw, server_hostname=self.host) if self.tls else raw
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(('GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (self.host, key)).encode())
        head = b''
        while b'\r\n\r\n' not in head:
            d = s.recv(1024)
            if not d:
                raise RuntimeError('the device closed the connection')
            head += d
        if b' 101 ' not in head.split(b'\r\n')[0]:
            raise RuntimeError('WebSocket refused: ' + head.split(b'\r\n')[0].decode('latin1'))
        rest = head.split(b'\r\n\r\n', 1)[1]
        s.settimeout(None)
        self.sock = s
        threading.Thread(target=self._reader, args=(s, rest), daemon=True).start()
        self._send('NET')                      # the device registers a client with its first frame
        time.sleep(1.0)                        # ... and its answer must not end up in the output of the first real command
        log('connected to', self.host)

    def _send(self, text):
        data = text.encode()
        mk = os.urandom(4)
        n = len(data)
        hdr = bytes([0x81, 0x80 | n]) if n < 126 else bytes([0x81, 0x80 | 126]) + struct.pack('>H', n)
        self.sock.sendall(hdr + mk + bytes(b ^ mk[i % 4] for i, b in enumerate(data)))

    def _reader(self, s, buf):
        def need(n):
            nonlocal buf
            while len(buf) < n:
                d = s.recv(4096)
                if not d:
                    raise EOFError
                buf += d
            out, buf = buf[:n], buf[n:]
            return out
        try:
            while True:
                b0, b1 = need(2)
                op, ln = b0 & 15, b1 & 127
                if ln == 126:
                    ln = struct.unpack('>H', need(2))[0]
                elif ln == 127:
                    ln = struct.unpack('>Q', need(8))[0]
                payload = need(ln)
                if op == 8:
                    break
                if op == 9:
                    s.sendall(bytes([0x8A, 0x80]) + b'\0\0\0\0')
                    continue
                if op == 1:
                    self._on_text(payload.decode('utf-8', 'replace'))
        except Exception:
            pass
        finally:
            if self.sock is s:
                self.sock = None
            try:
                s.close()
            except Exception:
                pass

    def _on_text(self, t):
        self.last = time.time()
        if t.startswith('{'):
            try:
                j = json.loads(t)
            except ValueError:
                return
            if j.get('t') == 'state':
                changed = j != self.state
                self.state = j
                if changed:
                    for f in list(self.listeners):
                        try:
                            f()
                        except Exception:
                            pass
            return
        with self.cv:
            self.seq += 1
            self.lines.append((self.seq, time.time(), t.rstrip('\r')))
            self.cv.notify_all()

    def ensure_ws(self):
        if self.sock is None:
            self._connect()

    def since(self, seq):
        return [l for (n, _, l) in list(self.lines) if n > seq]

    def run(self, cmd, wait=4.0, settle=0.8):
        """send one console command, return the lines the device wrote until it said OK / ERR (plus a short settle for the amplifier's answers)"""
        if self.dry:
            log('DRY-RUN', cmd)
            return ['DRY-RUN: ' + cmd]
        with self.lock:
            self.ensure_ws()
            start = self.seq
            self._send(cmd)
            t0 = time.time()
            done = None
            while time.time() - t0 < wait:
                with self.cv:
                    self.cv.wait(0.1)
                new = self.since(start)
                if done is None and any(l.startswith(('OK', 'ERR')) for l in new):
                    done = time.time()
                if done is not None and time.time() - done >= settle:
                    break
            return self.since(start)

    def status(self):
        try:
            self.state = self.get_json('/api/state')
        except Exception:
            if self.state is None:
                raise
        return self.state


# ---------------------------------------------------------------------------------------------------------------- tools
class Tools:
    def __init__(self, dev, max_volume, allow_raw, allow_admin):
        self.dev, self.cap, self.raw, self.admin = dev, max_volume, allow_raw, allow_admin

    # the knob estimate: percent of the travel (None when unknown)
    def volume(self, st):
        v = st.get('vol10', -1)
        return None if v is None or v < 0 else v / 10.0

    def summary(self, st):
        v = self.volume(st)
        tun = st.get('tuner') or {}
        return {
            'power': {1: 'on', 0: 'standby'}.get(st.get('power'), 'unknown'),
            'muted': {1: True, 0: False}.get(st.get('mute')),
            'input': st.get('inName') or None,
            'volume_percent_of_knob': v,
            'volume_cap_percent': self.cap,
            'sound_field': st.get('sfName'),
            'amplifier_name': st.get('name') or None,
            'tuner': {'band': tun.get('band'), 'frequency': (tun.get('f', 0) / 100.0 if tun.get('band') == 'FM' else tun.get('f')), 'unit': 'MHz' if tun.get('band') == 'FM' else 'kHz',
                      'station_name': tun.get('name') or None, 'preset': None if tun.get('preset') in (None, 255) else tun.get('preset'), 'bank': tun.get('bank'),
                      'stereo': bool(tun.get('stereo')), 'locked': bool(tun.get('locked'))},
            'input_labels': st.get('labels') or {},
        }

    def out(self, lines, ok_prefix=('OK',)):
        err = [l for l in lines if l.startswith('ERR')]
        ok = (not err and any(l.startswith(ok_prefix) for l in lines)) or (self.dev.dry and bool(lines))
        return {'device_output': lines, 'ok': ok, 'errors': err}

    def send(self, cmd, **kw):
        return self.out(self.dev.run(cmd, **kw))

    # ---- readers
    def get_status(self, a):
        st = self.dev.status()
        return self.summary(st)

    def get_radio(self, a):
        d = {'tuner': self.summary(self.dev.status())['tuner']}
        for k, p in (('amplifier_presets', '/api/presets'), ('stored_stations', '/api/stations'), ('favorites', '/api/favs')):
            try:
                d[k] = self.dev.get_json(p)
            except Exception as e:
                d[k] = {'error': str(e)}
        return d

    def get_device_info(self, a):
        s = self.dev.get_json('/api/stats')
        keep = ('fw', 'built', 'chip', 'rev', 'up', 'reset', 'heap', 'heapMin', 'psram', 'psramFree', 'temp', 'host', 'mac', 'ssid', 'ble', 'bleName', 'bleMac', 'bleDiag', 'web', 'tcp', 'eco', 'idf')
        d = {k: s.get(k) for k in keep if k in s}
        if isinstance(s.get('wifi'), dict):
            d['wifi'] = s['wifi']
        if isinstance(s.get('bus'), dict):
            d['bus'] = s['bus']
        return d

    def refresh_status(self, a):
        r = self.send('STATUS', wait=6, settle=1.5)
        r['status'] = self.summary(self.dev.status())
        return r

    def recent_log(self, a):
        n = max(1, min(int(a.get('lines', 40)), 400))
        return {'lines': [l for (_, _, l) in list(self.dev.lines)[-n:]]}

    # ---- amplifier
    def power(self, a):
        on = bool(a['on'])
        r = self.send('PWRON' if on else 'PWROFF', wait=3)
        r['requested'] = 'on' if on else 'standby'
        return r

    def _volume_guard(self):
        st = self.dev.status()
        if st.get('power') == 0:
            raise ValueError('The amplifier is in standby: the volume knob does not move. Turn it on first (power).')
        return self.volume(st)

    def set_volume(self, a):
        p = float(a['percent'])
        if p < 0:
            raise ValueError('percent must be >= 0')
        if p > self.cap:
            raise ValueError('Refused: %.1f %% is above the safety cap of %.1f %% of the knob travel (--max-volume).' % (p, self.cap))
        self._volume_guard()
        r = self.send('VOL SET %.1f' % p, wait=30, settle=0.3)       # the motor is slow, and from an unknown position it first goes to the minimum
        r['requested_percent'] = p
        return r

    def change_volume(self, a):
        up = a['direction'] == 'up'
        n = max(1, min(int(a.get('steps', 4)), 20))
        cur = self._volume_guard()
        if up:
            if cur is None:
                raise ValueError('Refused: the knob position is not known, so the cap cannot be checked. Use set_volume (it goes to the minimum first).')
            if cur + n * (100.0 / 420) > self.cap + 0.01:
                raise ValueError('Refused: %d steps up from about %.1f %% would pass the safety cap of %.1f %%.' % (n, cur, self.cap))
        r = self.send('VOL %s %d' % ('UP' if up else 'DOWN', n), wait=12, settle=0.3)
        r['steps'] = n
        return r

    def mute(self, a):
        return self.send('MUTE' if a['muted'] else 'UNMUTE', wait=3)

    def select_input(self, a):
        i = str(a['input']).upper()
        if i not in INPUTS:
            raise ValueError('input must be one of: ' + ', '.join(INPUTS))
        return self.send('INPUT ' + i, wait=4)

    def tune(self, a):
        band, f = str(a['band']).upper(), float(a['frequency'])
        if band == 'FM':
            if not 87.5 <= f <= 108.0:
                raise ValueError('FM frequency in MHz, 87.5 to 108')
            return self.send('TUNE FM %.2f' % f, wait=5)
        if band == 'AM':
            if not 530 <= f <= 1710:
                raise ValueError('AM frequency in kHz, 530 to 1710')
            return self.send('TUNE AM %d' % round(f), wait=5)
        raise ValueError('band must be FM or AM')

    def tune_station(self, a):
        s = re.sub(r'[^\w #.+\-]', '', str(a['station'])).strip()
        if not s:
            raise ValueError('station is empty')
        return self.send('TUNE ' + s, wait=6)

    def recall_preset(self, a):
        b, n = int(a['bank']), int(a['number'])
        if not (1 <= b <= 3 and 1 <= n <= 10):
            raise ValueError('bank 1-3, number 1-10')
        return self.send('PRESET %d %d' % (b, n), wait=5)

    def seek(self, a):
        return self.send('TUNER SCAN+' if a['direction'] == 'up' else 'TUNER SCAN-', wait=8)

    def set_sound_field(self, a):
        n = int(a['field'])
        if not 0 <= n <= 29:
            raise ValueError('field 0 (off) to 29')
        return self.send('SF OFF' if n == 0 else 'SF %d' % n, wait=5)

    def get_eq(self, a):
        return self.send('EQ', wait=8, settle=1.0)

    def set_eq(self, a):
        side, band, g = str(a['side']).upper()[:1], str(a['band']).upper(), int(a['gain_db'])
        if side not in ('F', 'R') or band not in ('BASS', 'MID', 'TREBLE') or not -10 <= g <= 10:
            raise ValueError('side front|rear, band bass|mid|treble, gain_db -10..10')
        return self.send('EQ %s %s GAIN %d' % (side, band, g), wait=5)

    def all_off(self, a):
        return self.send('ALLOFF', wait=10, settle=1.0)

    def console_command(self, a):
        cmd = ' '.join(str(a['command']).split())
        if not cmd:
            raise ValueError('empty command')
        w = cmd.split()[0].upper()
        if HEX.match(cmd):
            raise ValueError('Raw bus frames are not accepted here: use send_raw_frame (needs --allow-raw).')
        if w in BLOCKED and not self.admin:
            raise ValueError('"%s" changes the network, the security, the firmware or the volume (use the dedicated tools): refused. Start the server with --allow-admin to allow it.' % w)
        return self.send(cmd, wait=float(a.get('wait_seconds', 6)), settle=1.0)

    def send_raw_frame(self, a):
        if not self.raw:
            raise ValueError('Raw frames are off. Start the server with --allow-raw to allow them (they go straight to the S-Link bus).')
        h = ' '.join(re.findall(r'[0-9A-Fa-f]{2}', str(a['hex']))).upper()
        if not h or len(h.split()) > 24:
            raise ValueError('hex bytes, 1 to 24 (e.g. "C0 2E")')
        return self.send(h, wait=4)


def S(props, req=()):
    return {'type': 'object', 'properties': props, 'required': list(req), 'additionalProperties': False}


NUM = lambda d, **k: dict({'type': 'number', 'description': d}, **k)
ENUM = lambda d, v: {'type': 'string', 'enum': v, 'description': d}
RO = {'readOnlyHint': True, 'openWorldHint': False}
WR = lambda destructive=False, idem=False: {'readOnlyHint': False, 'destructiveHint': destructive, 'idempotentHint': idem, 'openWorldHint': False}

TOOLS = [
    ('get_status', 'Amplifier status', 'Current state of the Sony amplifier: power, mute, input, volume (estimate, % of the knob travel), sound field, tuner (band, frequency, RDS name, preset), input labels. The volume is an estimate: the motor knob does not report its position.', S({}), RO),
    ('get_radio', 'Radio information', 'The tuner now, the amplifier presets (3 banks x 10), the FM stations stored by the bridge and the bridge favourites.', S({}), RO),
    ('get_device_info', 'Bridge information', 'The S-Link bridge itself: firmware, uptime, memory, WiFi, Bluetooth, S-Link bus counters.', S({}), RO),
    ('refresh_status', 'Read the amplifier again', 'Ask the amplifier, tuner and surround processor for their state on the S-Link bus (takes a few seconds) and return the new status.', S({}), WR(False, True)),
    ('recent_log', 'Recent console output', 'The last lines the bridge wrote (answers, decoded S-Link frames).', S({'lines': {'type': 'integer', 'minimum': 1, 'maximum': 400, 'description': 'how many (default 40)'}}), RO),
    ('power', 'Power on / standby', 'Switch the amplifier on or put it in standby.', S({'on': {'type': 'boolean', 'description': 'true = on, false = standby'}}, ['on']), WR(False, True)),
    ('set_volume', 'Set the volume', 'Set the volume as a percent of the motor knob travel (0 = minimum). Never above the safety cap (default 30). The knob is slow (tens of seconds for a big change); from an unknown position it first goes to the minimum. Refused in standby.', S({'percent': NUM('0 to the cap', minimum=0, maximum=100)}, ['percent']), WR(False, True)),
    ('change_volume', 'Volume up / down by steps', 'Move the knob a few steps (about 0.24 % of the travel each). Going up is refused when the cap would be passed or the position is unknown.', S({'direction': ENUM('up or down', ['up', 'down']), 'steps': {'type': 'integer', 'minimum': 1, 'maximum': 20, 'description': 'default 4'}}, ['direction']), WR()),
    ('mute', 'Mute / unmute', 'Mute or unmute the amplifier.', S({'muted': {'type': 'boolean'}}, ['muted']), WR(False, True)),
    ('select_input', 'Select the input', 'Choose the source: ' + ', '.join(INPUTS) + '.', S({'input': ENUM('source', INPUTS)}, ['input']), WR(False, True)),
    ('tune', 'Tune to a frequency', 'Tune the tuner to a frequency (the TUNER input must be selected).', S({'band': ENUM('FM or AM', ['FM', 'AM']), 'frequency': NUM('FM in MHz (87.5-108), AM in kHz (530-1710)')}, ['band', 'frequency']), WR(False, True)),
    ('tune_station', 'Tune to a stored station', 'Tune to a station stored by the bridge, by (part of) its name or by its number ("#3").', S({'station': {'type': 'string', 'description': 'name or #number'}}, ['station']), WR(False, True)),
    ('recall_preset', 'Recall an amplifier preset', 'Recall one of the amplifier\'s own tuner presets.', S({'bank': {'type': 'integer', 'minimum': 1, 'maximum': 3}, 'number': {'type': 'integer', 'minimum': 1, 'maximum': 10}}, ['bank', 'number']), WR(False, True)),
    ('seek', 'Seek the next station', 'Scan up or down to the next station.', S({'direction': ENUM('up or down', ['up', 'down'])}, ['direction']), WR()),
    ('set_sound_field', 'Sound field', 'Choose a sound field (DSP mode) 1-29, or 0 for off.', S({'field': {'type': 'integer', 'minimum': 0, 'maximum': 29}}, ['field']), WR(False, True)),
    ('get_eq', 'Read the equaliser', 'Read bass / mid / treble of the front and rear equaliser (takes a few seconds).', S({}), WR(False, True)),
    ('set_eq', 'Set an equaliser band', 'Set the gain of one band of the 3-band equaliser.', S({'side': ENUM('front or rear', ['front', 'rear']), 'band': ENUM('band', ['bass', 'mid', 'treble']), 'gain_db': {'type': 'integer', 'minimum': -10, 'maximum': 10}}, ['side', 'band', 'gain_db']), WR(False, True)),
    ('all_off', 'Turn off every unit', 'Power off every Sony unit on the bus (CD, MD, tuner, then the amplifier).', S({}), WR(True, True)),
    ('console_command', 'Bridge console command', 'Send any other command of the bridge console (STATUS, MODEL, INPUTS, PRESETS, FAV, EQCURVE, SF ..., TUNER ...) and return its output. Commands that change the network, security, firmware or volume are refused (use the dedicated tools).',
     S({'command': {'type': 'string', 'description': 'e.g. "PRESETS 1"'}, 'wait_seconds': {'type': 'number', 'minimum': 1, 'maximum': 60}}, ['command']), WR(True)),
    ('send_raw_frame', 'Raw S-Link frame', 'Send a raw frame to the S-Link bus (hex bytes, e.g. "C0 2E"). Disabled unless the server was started with --allow-raw.', S({'hex': {'type': 'string'}}, ['hex']), WR(True)),
]

RESOURCES = [
    ('slink://state', 'Amplifier state', 'The raw state frame of the bridge (JSON).', 'application/json'),
    ('slink://radio/presets', 'Amplifier tuner presets', 'The amplifier\'s own presets (3 banks).', 'application/json'),
    ('slink://radio/stations', 'FM stations', 'FM stations stored by the bridge.', 'application/json'),
    ('slink://radio/favorites', 'Favourites', 'Favourite stations kept by the bridge.', 'application/json'),
    ('slink://device/stats', 'Bridge statistics', 'Firmware, memory, WiFi, Bluetooth and bus counters.', 'application/json'),
    ('slink://device/help', 'Console commands', 'The HELP text of the bridge console.', 'text/plain'),
    ('slink://log', 'Console log', 'The last lines written by the bridge.', 'text/plain'),
]

PROMPTS = {
    'radio_evening': ('Evening radio', 'Switch the amplifier on, select the tuner and a station, at a comfortable volume.', [('station', 'station name or FM frequency (optional)', False)],
                      lambda a: 'Please get the amplifier ready for some radio. Check get_status first. If it is in standby, switch it on; select the TUNER input; ' +
                                ('tune to "%s" (a stored station name or an FM frequency in MHz)' % a['station'] if a.get('station') else 'pick a good stored station from get_radio') +
                                '; then set a moderate volume (about 15 %) with set_volume. Tell me what you did and what is playing.'),
    'check_system': ('Check the system', 'Summarise the state of the amplifier and of the bridge.', [],
                     lambda a: 'Use get_status, get_device_info and recent_log to give me a short health report of the amplifier and the S-Link bridge: power, input, volume, tuner, firmware, memory, WiFi, Bluetooth, and anything that looks wrong.'),
    'safe_listening': ('Safe listening level', 'Bring the volume to a quiet, safe level.', [('percent', 'target percent of the knob (default 10)', False)],
                       lambda a: 'Read get_status. If the amplifier is on, set the volume to %s %% of the knob with set_volume (never above the cap) and confirm. If it is in standby, say so and do not change anything.' % (a.get('percent') or '10')),
}


# ---------------------------------------------------------------------------------------------------------------- MCP
class Server:
    def __init__(self, dev, tools, can_subscribe):
        self.dev, self.t, self.can_subscribe = dev, tools, can_subscribe
        self.subs = set()
        self.notify = lambda msg: None
        self.index = {t[0]: getattr(tools, t[0]) for t in TOOLS}

    def tools_list(self):
        return [{'name': n, 'title': title, 'description': desc, 'inputSchema': schema, 'annotations': ann} for (n, title, desc, schema, ann) in TOOLS]

    def call_tool(self, name, args):
        if name not in self.index:
            raise KeyError(name)
        try:
            res = self.index[name](args or {})
            text = json.dumps(res, ensure_ascii=False, indent=2)
            r = {'content': [{'type': 'text', 'text': text}]}
            if isinstance(res, dict):
                r['structuredContent'] = res
            if isinstance(res, dict) and res.get('errors'):
                r['isError'] = True
            return r
        except KeyError as e:
            return {'content': [{'type': 'text', 'text': 'Missing argument: %s' % e}], 'isError': True}
        except (ValueError, TypeError) as e:
            return {'content': [{'type': 'text', 'text': str(e)}], 'isError': True}
        except Exception as e:
            return {'content': [{'type': 'text', 'text': 'The bridge did not answer (%s: %s). Is it on, and is %s the right address?' % (type(e).__name__, e, self.dev.host)}], 'isError': True}

    def read_resource(self, uri):
        d = self.dev
        if uri == 'slink://state':
            return 'application/json', json.dumps(d.status(), indent=2)
        if uri == 'slink://radio/presets':
            return 'application/json', d.get('/api/presets')
        if uri == 'slink://radio/stations':
            return 'application/json', d.get('/api/stations')
        if uri == 'slink://radio/favorites':
            return 'application/json', d.get('/api/favs')
        if uri == 'slink://device/stats':
            return 'application/json', d.get('/api/stats')
        if uri == 'slink://device/help':
            return 'text/plain', '\n'.join(d.run('HELP', wait=6, settle=1.0))
        if uri == 'slink://log':
            return 'text/plain', '\n'.join(l for (_, _, l) in list(d.lines)[-100:])
        raise KeyError(uri)

    def handle(self, msg):
        """one JSON-RPC message -> the response dict, or None for a notification"""
        if not isinstance(msg, dict) or msg.get('jsonrpc') != '2.0':
            return {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32600, 'message': 'Invalid request'}}
        mid, method, p = msg.get('id'), msg.get('method'), msg.get('params') or {}
        isnote = 'id' not in msg
        try:
            res = self.dispatch(method, p)
        except KeyError as e:
            err = {'code': -32602, 'message': 'Unknown name or URI: %s' % e}
        except LookupError as e:
            err = {'code': -32601, 'message': 'Method not found: %s' % e}
        except Exception as e:
            err = {'code': -32603, 'message': '%s: %s' % (type(e).__name__, e)}
        else:
            return None if isnote else {'jsonrpc': '2.0', 'id': mid, 'result': res}
        return None if isnote else {'jsonrpc': '2.0', 'id': mid, 'error': err}

    def dispatch(self, m, p):
        if m == 'initialize':
            want = p.get('protocolVersion')
            caps = {'tools': {'listChanged': False}, 'resources': {'subscribe': self.can_subscribe, 'listChanged': False}, 'prompts': {'listChanged': False}, 'logging': {}}
            return {'protocolVersion': want if want in PROTOCOLS else PROTOCOLS[0], 'capabilities': caps,
                    'serverInfo': {'name': 'slink', 'title': 'Sony S-Link bridge', 'version': VERSION},
                    'instructions': 'Controls a Sony amplifier (S-Link / Control-A1) through an ESP32 bridge. Start with get_status. The volume is an estimate in %% of the knob travel and is capped at %g %% for safety; '
                                    'the knob is slow. The amplifier answers only while it is on; the tuner only while the TUNER input is selected. Prefer the dedicated tools to console_command.' % self.t.cap}
        if m in ('notifications/initialized', 'notifications/cancelled', 'notifications/roots/list_changed'):
            return None
        if m == 'ping':
            return {}
        if m == 'tools/list':
            return {'tools': self.tools_list()}
        if m == 'tools/call':
            return self.call_tool(p.get('name'), p.get('arguments'))
        if m == 'resources/list':
            return {'resources': [{'uri': u, 'name': n, 'title': n, 'description': d, 'mimeType': t} for (u, n, d, t) in RESOURCES]}
        if m == 'resources/templates/list':
            return {'resourceTemplates': []}
        if m == 'resources/read':
            uri = p['uri']
            mime, text = self.read_resource(uri)
            return {'contents': [{'uri': uri, 'mimeType': mime, 'text': text}]}
        if m == 'resources/subscribe':
            if not self.can_subscribe:
                raise LookupError(m)
            self.subs.add(p['uri'])
            return {}
        if m == 'resources/unsubscribe':
            self.subs.discard(p.get('uri'))
            return {}
        if m == 'prompts/list':
            return {'prompts': [{'name': k, 'title': v[0], 'description': v[1], 'arguments': [{'name': a, 'description': d, 'required': r} for (a, d, r) in v[2]]} for k, v in PROMPTS.items()]}
        if m == 'prompts/get':
            v = PROMPTS[p['name']]
            return {'description': v[1], 'messages': [{'role': 'user', 'content': {'type': 'text', 'text': v[3](p.get('arguments') or {})}}]}
        if m == 'logging/setLevel':
            return {}
        raise LookupError(m)


# ---------------------------------------------------------------------------------------------------------------- transports
def run_stdio(srv, dev):
    out_lock = threading.Lock()

    def write(obj):
        with out_lock:
            sys.stdout.write(json.dumps(obj, ensure_ascii=False, separators=(',', ':')) + '\n')
            sys.stdout.flush()
    srv.notify = write

    def on_state():
        if 'slink://state' in srv.subs:
            write({'jsonrpc': '2.0', 'method': 'notifications/resources/updated', 'params': {'uri': 'slink://state'}})
    dev.listeners.append(on_state)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            write({'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'Parse error'}})
            continue
        for m in (msg if isinstance(msg, list) else [msg]):
            r = srv.handle(m)
            if r is not None:
                write(r)


def run_http(srv, bind, port, token):
    class H(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, f, *a):
            log(self.address_string(), f % a)

        def reply(self, code, body=b'', ctype='application/json', extra=None):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def allowed(self):
            o = self.headers.get('Origin')
            if o and not re.match(r'^https?://(localhost|127\.0\.0\.1|\[::1\])(:\d+)?$', o) and bind in ('127.0.0.1', 'localhost'):
                self.reply(403, b'{"error":"origin not allowed"}')
                return False
            if token and self.headers.get('Authorization') != 'Bearer ' + token:
                self.reply(401, b'{"error":"bearer token required"}', extra={'WWW-Authenticate': 'Bearer'})
                return False
            return True

        def do_GET(self):
            self.reply(405, b'{"error":"POST JSON-RPC messages to /mcp"}', extra={'Allow': 'POST'})

        def do_POST(self):
            if self.path.split('?')[0] != '/mcp':
                return self.reply(404, b'{"error":"use /mcp"}')
            if not self.allowed():
                return
            try:
                msg = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'null')
            except ValueError:
                return self.reply(400, b'{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}}')
            res = [r for r in (srv.handle(m) for m in (msg if isinstance(msg, list) else [msg])) if r is not None]
            if not res:
                return self.reply(202)
            self.reply(200, json.dumps(res if isinstance(msg, list) else res[0], ensure_ascii=False).encode())

    httpd = ThreadingHTTPServer((bind, port), H)
    log('MCP over HTTP at http://%s:%d/mcp%s' % (bind, port, ' (bearer token required)' if token else ''))
    httpd.serve_forever()


def main():
    ap = argparse.ArgumentParser(description='MCP server for the S-Link bridge', formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('--host', default=os.environ.get('SLINK_HOST', 'slink.local'), help='address of the bridge (default slink.local or $SLINK_HOST)')
    ap.add_argument('--tls', action='store_true', default=os.environ.get('SLINK_TLS') == '1', help='use https/wss (the bridge\'s HTTPS on port 443)')
    ap.add_argument('--cafile', default=os.environ.get('SLINK_CA'), help='CA certificate of the bridge (slink-ca.crt) for --tls')
    ap.add_argument('--http', type=int, metavar='PORT', help='serve Streamable HTTP on /mcp instead of stdio')
    ap.add_argument('--bind', default='127.0.0.1', help='address of the HTTP mode (default 127.0.0.1)')
    ap.add_argument('--token', default=os.environ.get('SLINK_MCP_TOKEN'), help='bearer token required by the HTTP mode')
    ap.add_argument('--max-volume', type=float, default=float(os.environ.get('SLINK_MAX_VOLUME', 30)), help='volume cap, %% of the knob travel (default 30)')
    ap.add_argument('--allow-raw', action='store_true', default=os.environ.get('SLINK_MCP_RAW') == '1', help='enable send_raw_frame')
    ap.add_argument('--allow-admin', action='store_true', default=os.environ.get('SLINK_MCP_ADMIN') == '1', help='let console_command run network / security / firmware / volume commands')
    ap.add_argument('--dry-run', action='store_true', default=os.environ.get('SLINK_DRY_RUN') == '1', help='log the commands instead of sending them')
    a = ap.parse_args()
    if a.http and a.bind not in ('127.0.0.1', 'localhost', '::1') and not a.token:
        ap.error('--bind to a network address needs --token (the bridge has no login of its own)')
    dev = Device(a.host, a.tls, a.cafile, a.dry_run)
    srv = Server(dev, Tools(dev, a.max_volume, a.allow_raw, a.allow_admin), can_subscribe=not a.http)
    log('bridge %s%s, volume cap %g %%%s' % (a.host, ' (tls)' if a.tls else '', a.max_volume, ', DRY RUN' if a.dry_run else ''))
    if a.http:
        run_http(srv, a.bind, a.http, a.token)
    else:
        run_stdio(srv, dev)


if __name__ == '__main__':
    main()
