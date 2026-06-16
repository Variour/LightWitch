#!/usr/bin/env python3
"""Serve the Battery Light web UI locally with lightweight mock APIs.

Usage:
  python3 scripts/serve_local.py
  python3 scripts/serve_local.py --host 127.0.0.1 --port 8080
"""

import argparse
import json
import socketserver
from http.server import BaseHTTPRequestHandler, SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs


PROJECT_DIR = Path(__file__).resolve().parents[1]
DATA_DIR = PROJECT_DIR / "data"
SCENES_DIR = DATA_DIR / "scenes"


class LocalHandler(SimpleHTTPRequestHandler):
    _scenes = {}

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DATA_DIR), **kwargs)
        self._ensure_scene_store()
        self._load_scene_files()

    def log_message(self, format, *args):
        print(f"[local] {self.address_string()} - - [{self.log_date_time_string()}] {format % args}")

    def do_GET(self):
        if self._handle_api_get():
            return
        super().do_GET()

    def do_POST(self):
        if self._handle_api_post():
            return
        self.send_error(501, "Unsupported method ('POST')")

    def _ensure_scene_store(self):
        SCENES_DIR.mkdir(exist_ok=True)

    def _scene_path(self, scene_id):
        return SCENES_DIR / f"{scene_id}.json"

    def _load_scene_files(self):
        self._ensure_scene_store()
        self.__class__._scenes = {}
        if not SCENES_DIR.exists():
            return

        for path in sorted(SCENES_DIR.glob('*.json')):
            try:
                payload = json.loads(path.read_text(encoding='utf-8'))
            except (OSError, ValueError, TypeError):
                continue

            if isinstance(payload, dict) and payload.get('id'):
                self.__class__._scenes[payload['id']] = payload

    def _write_scene_file(self, scene_id, payload):
        self._ensure_scene_store()
        path = self._scene_path(scene_id)
        path.write_text(json.dumps(payload, separators=(',', ':')), encoding='utf-8')
        self.__class__._scenes[scene_id] = payload

    def _remove_scene_file(self, scene_id):
        path = self._scene_path(scene_id)
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        self.__class__._scenes.pop(scene_id, None)

    def _handle_api_get(self):
        path = urlsplit(self.path).path
        if not path.startswith('/api/'):
            return False

        if path == '/api/config':
            self._send_json(200, {
                "deviceName": "Local Dev",
                "wifiSsid": "",
                "otaPort": 3232,
                "ledType": 0,
                "logLevel": 1,
                "mqttHost": "",
                "mqttPort": 1883,
                "mqttUser": "",
                "version": "local-dev",
                "mac": "11:22:33:44:55:66",
                "groups": [],
            })
            return True

        if path == '/api/peers':
            self._send_json(200, {
                "self": {
                    "name": "Local Dev",
                    "mac": "11:22:33:44:55:66",
                    "groupId": 0,
                    "online": True,
                },
                "peers": [],
            })
            return True

        if path == '/api/scenes':
            scenes = [
                {"id": scene_id, "name": scene['name'], "w": scene['w'], "h": scene['h'], "fc": len(scene['frames'])}
                for scene_id, scene in self._scenes.items()
            ]
            self._send_json(200, {"scenes": scenes})
            return True

        if path == '/api/scenes/get':
            params = parse_qs(urlsplit(self.path).query)
            scene_id = params.get('id', [''])[0]
            scene = self._scenes.get(scene_id)
            if not scene:
                self._send_json(404, {"error": "not found"})
            else:
                self._send_json(200, scene)
            return True

        self._send_json(404, {"error": "not found"})
        return True

    def _handle_api_post(self):
        path = urlsplit(self.path).path
        if not path.startswith('/api/'):
            return False

        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length) if length else b''

        try:
            payload = json.loads(body.decode('utf-8')) if body else {}
        except json.JSONDecodeError:
            self._send_json(400, {"error": "bad json"})
            return True

        if path == '/api/scenes/create':
            name = payload.get('name', 'Unnamed')
            w = int(payload.get('w', 20) or 20)
            h = int(payload.get('h', 10) or 10)

            scene_id = f"local-{len(self._scenes) + 1:04d}"
            while scene_id in self._scenes:
                scene_id = f"local-{int(scene_id.split('-')[-1]) + 1:04d}"

            scene = {
                "id": scene_id,
                "name": name,
                "w": w,
                "h": h,
                "fc": 0,
                "frames": [],
            }
            self._write_scene_file(scene_id, scene)
            self._send_json(200, {"ok": True, "id": scene_id})
            return True

        if path == '/api/scenes/save':
            scene_id = payload.get('id')
            if not scene_id:
                self._send_json(400, {"error": "missing id"})
                return True

            payload = dict(payload)
            payload.setdefault('fc', len(payload.get('frames', [])))
            self._write_scene_file(scene_id, payload)
            self._send_json(200, {"ok": True})
            return True

        if path == '/api/scenes/delete':
            scene_id = payload.get('id')
            if scene_id:
                self._remove_scene_file(scene_id)
            self._send_json(200, {"ok": True})
            return True

        if path == '/api/peers/setgroup':
            self._send_json(200, {"ok": True})
            return True

        if path in ('/api/groups/create', '/api/groups/update', '/api/groups/delete'):
            self._send_json(200, {"ok": True})
            return True

        self._send_json(404, {"error": "not found"})
        return True

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description="Serve the Battery Light web UI locally")
    parser.add_argument("--host", default="127.0.0.1", help="Host to bind to")
    parser.add_argument("--port", type=int, default=8080, help="Port to bind to")
    args = parser.parse_args()

    if not DATA_DIR.exists():
        raise SystemExit(f"Data directory not found: {DATA_DIR}")

    with ThreadingHTTPServer((args.host, args.port), LocalHandler) as httpd:
        print(f"Serving Battery Light UI from {DATA_DIR}")
        print(f"Open http://{args.host}:{args.port}/")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping local server...")


if __name__ == "__main__":
    main()
