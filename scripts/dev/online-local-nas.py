#!/usr/bin/env python3
"""A local stand-in for the MH3SP account server, for testing online play
against a local MH3SP server (docs/online.md) without the community servers.

  online-local-nas.py [--host 127.0.0.1] [--http-port 18080]

Answers what the game asks before it reaches the Capcom server:

  * http://nas.mh3sp.com/ac (the Nintendo account server, DWC's NAS login):
    acctcreate, login and svcloc, as AltWFC's nas_server.py does
  * http://nas.mh3sp.com/pr (the profanity check): no word is bad
  * available.mh3sp.com, UDP 27900 (GameSpy's availability check): available

Run the game with MH3_ONLINE_HOSTS='*.mh3sp.com=127.0.0.1' and
MH3_ONLINE_PORTS=80=18080 so the account server's port 80 reaches this one.
"""
import argparse
import base64
import http.server
import random
import socketserver
import string
import threading
import time
import urllib.parse


def decode(query):
    """NAS form fields: base64 with '*' for '=' (and '.'/'-' variants)."""
    fields = {}

    for key, value in urllib.parse.parse_qsl(query, keep_blank_values=True):
        text = value.replace('*', '=').replace('.', '+').replace('>', '+').replace('-', '/').replace('?', '/')

        try:
            fields[key] = base64.b64decode(text).decode('latin-1')
        except Exception:
            fields[key] = value

    return fields


def encode(fields):
    return ('&'.join(f'{key}={base64.b64encode(value.encode()).decode().replace("=", "*")}' for key, value in fields.items()) + '\r\n').encode()


def token():
    return 'NDS' + ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(96))


class Nas(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('X-Organization', 'Nintendo')
        self.end_headers()
        self.wfile.write(b'ok')

    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        fields = decode(self.rfile.read(length).decode('latin-1'))
        print(f'NAS {self.path}: {fields}', flush=True)

        if self.path == '/ac':
            action = fields.get('action', '').lower()

            if action == 'acctcreate':
                reply = {'retry': '0', 'returncd': '002', 'userid': str(random.randint(100000000, 999999999))}
            elif action == 'login':
                reply = {'retry': '0', 'returncd': '001', 'locator': 'gamespy.com', 'challenge': ''.join(random.choice(string.ascii_uppercase) for _ in range(8)), 'token': token()}
            elif action == 'svcloc':
                reply = {'retry': '0', 'returncd': '007', 'statusdata': 'Y', 'svchost': 'n/a', 'servicetoken': token()}
            else:
                reply = {'retry': '0', 'returncd': '109'}

            reply['datetime'] = time.strftime('%Y%m%d%H%M%S')
        elif self.path == '/pr':
            words = len(fields.get('words', '').split('\t'))
            reply = {'prwords': '0' * words, 'returncd': '000', 'datetime': time.strftime('%Y%m%d%H%M%S')}

            for region in 'ACEJKP':
                reply['prwords' + region] = '0' * words
        else:
            self.send_response(404)
            self.end_headers()
            return

        body = encode(reply)
        print(f'NAS reply: {reply}', flush=True)
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('NODE', 'wifiappe1')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Available(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request

        # 0x09, four zero bytes, the game name: the backend is available.
        if data[:1] == b'\x09':
            print(f'available: {data[5:].split(b"\0")[0].decode(errors="replace")}', flush=True)
            sock.sendto(b'\xfe\xfd\x09\x00\x00\x00\x00', self.client_address)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--http-port', type=int, default=18080)
    args = parser.parse_args()
    http_server = http.server.ThreadingHTTPServer((args.host, args.http_port), Nas)
    udp_server = socketserver.UDPServer((args.host, 27900), Available)
    threading.Thread(target=udp_server.serve_forever, daemon=True).start()
    print(f'NAS on http://{args.host}:{args.http_port}, availability on UDP {args.host}:27900', flush=True)
    http_server.serve_forever()


if __name__ == '__main__':
    main()
