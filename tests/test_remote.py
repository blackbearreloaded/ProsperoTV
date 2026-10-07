# ProsperoTV - ps5-native-app-boilerplate HTTP remote integration tests.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
import http.client
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class RemoteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.build.name) / 'remote'
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++20', '-pthread',
                        '-Wall', '-Wextra', '-Werror', '-DIPTV_REMOTE_PAIRING_MS=1500',
                        '-Iinclude', 'src/iptv_remote.cpp', 'tests/remote_host.cpp',
                        '-o', str(cls.binary)], cwd=ROOT, check=True)
    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()
    def setUp(self):
        self.data = tempfile.TemporaryDirectory()
        self.store = pathlib.Path(self.data.name) / 'phones.txt'
        self.cookie = ''
        self.start()
    def tearDown(self):
        self.stop()
        self.data.cleanup()
    def start(self, port=0, playback=False):
        args = [str(self.binary), str(port), str(self.store)]
        if playback: args.append('playback')
        self.server = subprocess.Popen(args, cwd=ROOT, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, text=True, encoding='utf-8')
        self.port = int(self.server.stdout.readline().strip().rsplit(':', 1)[1])
    def stop(self):
        self.server.terminate()
        self.server.wait(timeout=5)
        self.server.stdin.close()
        self.server.stdout.close()
    def control(self, text):
        self.server.stdin.write(text + '\n')
        self.server.stdin.flush()
        return self.server.stdout.readline().strip()
    def request(self, path, body=None, code=200, header=True, cookie=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)
        headers = {'X-ProsperoTV-Remote': '1'} if header else {}
        token = self.cookie if cookie is None else cookie
        if token: headers['Cookie'] = token
        connection.request('GET' if body is None else 'POST', path,
                           body.encode() if isinstance(body, str) else body, headers)
        response = connection.getresponse()
        data = response.read()
        self.assertEqual(response.status, code, data[:200])
        self.assertIsNone(response.getheader('Access-Control-Allow-Origin'))
        self.headers = dict(response.getheaders())
        connection.close()
        return data if path == '/icon.png' else data.decode()
    def pair(self):
        pin = self.control('pair').split(':')[1]
        self.assertRegex(pin, r'^\d{6}$')
        self.request('/api/pair', pin)
        cookie = self.headers['Set-Cookie']
        self.assertIn('HttpOnly', cookie)
        self.assertIn('SameSite=Strict', cookie)
        self.assertIn('Max-Age=31536000', cookie)
        self.cookie = cookie.split(';', 1)[0]
        self.assertRegex(self.cookie, r'^prosperotv_remote=[0-9a-f]{64}$')
        return pin
    def test_pairing_is_explicit_expiring_and_single_use(self):
        self.request('/api/status', code=401)
        self.request('/api/pair', '123456', code=401)
        self.request('/api/pair', '123456', code=429)
        time.sleep(1.05)
        pin = self.control('pair').split(':')[1]
        time.sleep(1.6)
        self.request('/api/pair', pin, code=401)
        time.sleep(1.05)
        pin = self.pair()
        self.assertIn('Connected', self.request('/api/status'))
        self.request('/api/pair', pin, code=401)
        self.assertIn('Connected', self.request('/api/status'))
    def test_cancel_pairing(self):
        pin = self.control('pair').split(':')[1]
        self.assertEqual(self.control('cancel'), 'cancelled')
        self.request('/api/pair', pin, code=401)
    def test_connection_confirmation_requires_a_paired_browser(self):
        self.request('/api/status', code=401)
        self.assertEqual(self.control('connected'), 'connected:0')
        self.pair()
        self.assertEqual(self.control('connected'), 'connected:0')
        self.request('/api/status')
        self.assertEqual(self.control('connected'), 'connected:1')
        self.assertEqual(self.control('connected'), 'connected:0')
        self.control('pair')
        self.request('/api/volume')
        self.assertEqual(self.control('connected'), 'connected:0')
        self.request('/api/status')
        self.assertEqual(self.control('connected'), 'connected:1')
    def test_remembered_browser_survives_restart_and_port_change(self):
        self.pair()
        self.assertEqual(self.store.stat().st_mode & 0o777, 0o600)
        self.stop()
        self.start()
        self.assertIn('Connected', self.request('/api/status'))
        self.request('/api/status', code=401, cookie='prosperotv_remote=' + '0' * 64)
        self.request('/api/key', 'enter', code=403, header=False)
        self.assertEqual(self.control('forget'), 'forgot:1')
        self.request('/api/status', code=401)
        self.stop()
        self.start()
        self.request('/api/status', code=401)
    def test_forget_this_phone_revokes_only_its_token(self):
        self.pair()
        first = self.cookie
        self.pair()
        second = self.cookie
        self.assertNotEqual(first, second)
        self.request('/api/disconnect', '', cookie=first)
        self.assertIn('Max-Age=0', self.headers['Set-Cookie'])
        self.request('/api/status', code=401, cookie=first)
        self.assertIn('Connected', self.request('/api/status', cookie=second))
    def test_pairing_does_not_succeed_without_persistence(self):
        self.stop()
        self.store = pathlib.Path(self.data.name) / 'missing' / 'phones.txt'
        self.start()
        pin = self.control('pair').split(':')[1]
        self.request('/api/pair', pin, code=500)
        self.assertNotIn('Set-Cookie', self.headers)
    def test_phone_limit_does_not_evict_existing_browsers(self):
        for _ in range(8): self.pair()
        pin = self.control('pair').split(':')[1]
        self.request('/api/pair', pin, code=409)
        self.assertIn('Connected', self.request('/api/status'))
    def test_icon_and_page_are_public(self):
        self.assertIn('ProsperoTV', self.request('/', header=False))
        self.assertEqual(self.request('/icon.png', header=False),
                         (ROOT / 'opengl-ui/ps5/sce_sys/icon0.png').read_bytes())
        self.assertEqual(self.headers['Content-Type'], 'image/png')
    def test_volume_bounds_persistence_failure_and_auth(self):
        self.request('/api/volume', '0', code=401)
        self.pair()
        self.assertEqual(self.request('/api/volume'), '75')
        for value in ('0', '35', '100'):
            self.request('/api/volume', value)
            self.assertEqual(self.request('/api/volume'), value)
        for invalid in ('', '-1', '101', '1000', '1.5', 'hello'):
            self.request('/api/volume', invalid, code=400)
        self.request('/api/volume', '13', code=500)
        self.assertEqual(self.request('/api/volume'), '100')
    def test_navigation_search_and_http_framing(self):
        self.pair()
        for key, action in (('up', 8), ('down', 9), ('left', 10), ('right', 11),
                            ('enter', 0), ('back', 1), ('search', 3), ('favorite', 2),
                            ('previous', 5), ('next', 6)):
            self.request('/api/key', key)
            self.assertEqual(self.server.stdout.readline().strip(), f'key:{action}')
        for query in ('BBC News', 'café 日本', '', 'a' * 39):
            self.request('/api/search', query)
            self.assertEqual(self.server.stdout.readline().rstrip('\n'), f'search:{query}')
        for invalid in ('a' * 40, 'a\x00b', 'a\nb', b'\xc0\xaf', b'\xed\xa0\x80'):
            self.request('/api/search', invalid, code=400)
        self.request('/api/key', 'unknown', code=400)
        self.request('/missing', code=404)
        with socket.create_connection(('127.0.0.1', self.port)) as slow:
            slow.sendall(b'POST /api/search HTTP/1.1\r\n')
            self.assertIn('Connected', self.request('/api/status'))
            slow.sendall((f'X-ProsperoTV-Remote: 1\r\nCookie: {self.cookie}\r\n'
                          'Content-Length: 3\r\n\r\nB').encode())
            time.sleep(.02)
            slow.sendall(b'BC')
            self.assertIn(b'200 OK', slow.recv(4096))
            self.assertEqual(self.server.stdout.readline().strip(), 'search:BBC')
        for headers in ('Content-Length: 1\r\nContent-Length: 2',
                        'Transfer-Encoding: chunked', 'Content-Length: -1'):
            with socket.create_connection(('127.0.0.1', self.port)) as raw:
                raw.sendall(f'POST /api/key HTTP/1.1\r\n{headers}\r\n\r\nx'.encode())
                self.assertIn(b'400 Error', raw.recv(4096))
    def test_playback_favorite_and_volume(self):
        self.stop()
        self.start(playback=True)
        self.pair()
        self.request('/api/search', 'BBC', code=409)
        self.request('/api/volume', '25')
        self.assertEqual(self.request('/api/volume'), '25')
        for call, status, message in ((1, 200, 'Added to favorites'),
                                       (2, 200, 'Removed from favorites'),
                                       (3, 500, 'Could not save favorites')):
            self.assertIn(message, self.request('/api/key', 'favorite', code=status))
            self.assertEqual(self.server.stdout.readline().strip(), f'favorite:{call}')
        for key, action in (('back', 1), ('favorite', 2)):
            self.request('/api/key', key)
            self.assertEqual(self.server.stdout.readline().strip(), f'key:{action}')
    def test_unavailable_port_falls_back(self):
        self.stop()
        with socket.socket() as reserved:
            reserved.bind(('0.0.0.0', 0))
            reserved.listen(1)
            port = reserved.getsockname()[1]
            self.start(port=port)
            self.assertNotEqual(self.port, port)
            self.pair()

if __name__ == '__main__':
    unittest.main()
