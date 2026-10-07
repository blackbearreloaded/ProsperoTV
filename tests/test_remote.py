# ProsperoTV - ps5-native-app-boilerplate HTTP remote integration tests.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
import http.client
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class RemoteTest(unittest.TestCase):
    def test_production_server(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "remote"
            subprocess.run([shutil.which("clang++") or "c++", "-std=c++20", "-pthread",
                            "-Wall", "-Wextra", "-Werror", "-Iinclude",
                            "src/iptv_remote.cpp", "tests/remote_host.cpp", "-o", str(binary)],
                           cwd=ROOT, check=True)
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            server = subprocess.Popen([str(binary), str(port)], stdout=subprocess.PIPE,
                                      text=True, encoding="utf-8")
            try:
                pin = re.search(r"Code: (\d{6})", server.stdout.readline()).group(1)

                def request(path, body=None, code=200, authenticated=True):
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
                    headers = {"X-Remote-Pin": pin} if authenticated else {}
                    connection.request("GET" if body is None else "POST", path,
                                       body.encode("utf-8") if isinstance(body, str) else body,
                                       headers)
                    response = connection.getresponse()
                    result = response.read().decode("utf-8")
                    self.assertEqual(response.status, code, result)
                    self.assertIsNone(response.getheader("Access-Control-Allow-Origin"))
                    connection.close()
                    return result

                self.assertIn("ProsperoTV", request("/", authenticated=False))
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
                connection.request("GET", "/icon.png")
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Content-Type"), "image/png")
                self.assertEqual(response.read(), (ROOT / "opengl-ui/ps5/sce_sys/icon0.png").read_bytes())
                connection.close()
                request("/api/key", "enter", 401, False)
                time.sleep(1.05)
                self.assertIn("Connected", request("/api/status"))
                # A second instance must choose an available port, never fail
                # or steal the existing listener's address.
                other = subprocess.Popen([str(binary), str(port), "playback"], stdout=subprocess.PIPE,
                                         text=True, encoding="utf-8")
                try:
                    hint = other.stdout.readline()
                    actual = int(re.search(r":(\d+)  \|", hint).group(1))
                    other_pin = re.search(r"Code: (\d{6})", hint).group(1)
                    self.assertNotEqual(actual, port)
                    connection = http.client.HTTPConnection("127.0.0.1", actual, timeout=4)
                    connection.request("GET", "/")
                    response = connection.getresponse()
                    self.assertEqual(response.status, 200)
                    response.read()
                    connection.close()
                    for call, status, message in ((1, 200, "Added to favorites"),
                                                   (2, 200, "Removed from favorites"),
                                                   (3, 500, "Could not save favorites")):
                        connection = http.client.HTTPConnection("127.0.0.1", actual, timeout=4)
                        connection.request("POST", "/api/key", "favorite",
                                           {"X-Remote-Pin": other_pin})
                        response = connection.getresponse()
                        self.assertEqual(response.status, status)
                        self.assertIn(message, response.read().decode())
                        connection.close()
                        self.assertEqual(other.stdout.readline().strip(), f"favorite:{call}")
                    # Favorite must not also enqueue a key. Back ends playback;
                    # the next favorite belongs to the browser again.
                    for key, action in (("back", 1), ("favorite", 2)):
                        connection = http.client.HTTPConnection("127.0.0.1", actual, timeout=4)
                        connection.request("POST", "/api/key", key,
                                           {"X-Remote-Pin": other_pin})
                        response = connection.getresponse()
                        self.assertEqual(response.status, 200)
                        response.read()
                        connection.close()
                        self.assertEqual(other.stdout.readline().strip(), f"key:{action}")
                finally:
                    other.terminate()
                    other.wait(timeout=5)
                    other.stdout.close()
                for key, action in (("up", 8), ("down", 9), ("left", 10), ("right", 11),
                                    ("enter", 0), ("back", 1), ("search", 3),
                                    ("favorite", 2), ("previous", 5), ("next", 6)):
                    request("/api/key", key)
                    self.assertEqual(server.stdout.readline().strip(), f"key:{action}")
                for query in ("BBC News", "café 日本", "", "a" * 39):
                    request("/api/search", query)
                    self.assertEqual(server.stdout.readline().rstrip("\n"), f"search:{query}")
                for invalid in ("a" * 40, "a\x00b", "a\nb", b"\xc0\xaf", b"\xed\xa0\x80"):
                    request("/api/search", invalid, 400)
                request("/api/key", "unknown", 400)
                request("/missing", code=404)
                # Fragmented requests survive polls; a slow client cannot freeze others.
                with socket.create_connection(("127.0.0.1", port)) as slow:
                    slow.sendall(b"POST /api/search HTTP/1.1\r\n")
                    self.assertIn("Connected", request("/api/status"))
                    slow.sendall(f"X-Remote-Pin: {pin}\r\nContent-Length: 3\r\n\r\nB".encode())
                    time.sleep(.02)
                    slow.sendall(b"BC")
                    self.assertIn(b"200 OK", slow.recv(4096))
                    self.assertEqual(server.stdout.readline().strip(), "search:BBC")
                for headers in ("Content-Length: 1\r\nContent-Length: 2", "Transfer-Encoding: chunked", "Content-Length: -1"):
                    with socket.create_connection(("127.0.0.1", port)) as raw:
                        raw.sendall(f"POST /api/key HTTP/1.1\r\n{headers}\r\n\r\nx".encode())
                        self.assertIn(b"400 Error", raw.recv(4096))
                self.assertIn("Connected", request("/api/status"))
            finally:
                server.terminate()
                server.wait(timeout=5)
                server.stdout.close()


if __name__ == "__main__":
    unittest.main()
