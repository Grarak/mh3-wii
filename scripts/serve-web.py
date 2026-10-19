#!/usr/bin/env python3
"""Serve the web build on localhost.

    scripts/serve-web.py [BUILD_DIR] [--port 8080] [--disc IMAGE]

Pthreads need SharedArrayBuffer, which browsers enable only on cross-origin
isolated pages, so every response carries

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
    Cross-Origin-Resource-Policy: same-origin

WebGPU and the Origin Private File System need a secure context: localhost,
or HTTPS elsewhere. --disc also serves your disc image at /disc/NAME, so
http://localhost:8080/?disc=/disc/NAME copies it into the page's storage
without the file picker.
"""
import argparse
import functools
import http.server
import os
import shutil
import urllib.parse
from pathlib import Path


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.js': 'text/javascript'}
    disc = None

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def do_GET(self):
        if self.disc and urllib.parse.unquote(self.path.split('?')[0]) == '/disc/' + self.disc.name:
            size = self.disc.stat().st_size
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(size))
            self.end_headers()
            with self.disc.open('rb') as source:
                shutil.copyfileobj(source, self.wfile, 4 << 20)
            return
        super().do_GET()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('build', nargs='?', default='build/web', type=Path)
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--disc', type=Path, help='a disc image to offer at /disc/NAME')
    args = parser.parse_args()
    Handler.disc = args.disc.resolve() if args.disc else None
    handler = functools.partial(Handler, directory=os.fspath(args.build.resolve()))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), handler)
    print(f'Serving {args.build} at http://localhost:{args.port}/', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
