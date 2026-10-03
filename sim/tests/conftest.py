"""Fixtures: a virtual doorbell subprocess on ephemeral ports and a webhook sink."""
from __future__ import annotations

import http.client
import importlib.util
import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SIM = REPO / "sim" / "virtual_doorbell.py"
DBCMD = REPO / "tools" / "dbcmd.py"
TOKEN = "t3st-token"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = mod          # dataclasses look the module up by name
    spec.loader.exec_module(mod)
    return mod


dbcmd = load_module("dbcmd", DBCMD)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_until(pred, timeout: float, step: float = 0.05) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(step)
    return pred()


class WebhookSink:
    """Collects the JSON bodies POSTed by the simulator (like HA's webhook trigger would)."""

    def __init__(self) -> None:
        self.events: list[dict] = []
        self.cond = threading.Condition()
        sink = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
                body["_path"] = self.path
                with sink.cond:
                    sink.events.append(body)
                    sink.cond.notify_all()
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *a) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def hook(self, path: str = "/api/webhook/test") -> str:
        return "127.0.0.1:%d%s" % (self.port, path)

    def wait_for(self, event: str, timeout: float = 10.0, since: int = 0, **match) -> dict:
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for e in self.events[since:]:
                    if e.get("event") == event and all(e.get(k) == v for k, v in match.items()):
                        return e
                left = deadline - time.monotonic()
                if left <= 0:
                    raise AssertionError("no %r report (%s) within %.0f s; got %s" % (
                        event, match, timeout, [x.get("event") for x in self.events[since:]]))
                self.cond.wait(left)

    def mark(self) -> int:
        return len(self.events)

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


class Sim:
    def __init__(self, tmp: Path, sink: WebhookSink, speed: float = 1.0, extra: list[str] | None = None) -> None:
        self.tcp, self.http, self.ctl = free_port(), free_port(), free_port()
        self.state = tmp / "state.json"
        self.log = open(tmp / "sim.log", "w")
        args = [sys.executable, str(SIM), "--bind", "127.0.0.1", "--tcp-port", str(self.tcp), "--http-port", str(self.http),
                "--sim-port", str(self.ctl), "--state", str(self.state), "--hook", sink.hook(), "--token", TOKEN,
                "--fps", "5", "--time", str(speed), "--ip", "127.0.0.1"] + (extra or [])
        env = dict(os.environ)
        env.pop("DOORBELL_TOKEN", None)
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
        self.sink = sink
        self.start_mark = sink.mark()
        assert wait_until(lambda: port_open(self.tcp), 10), "simulator did not open its TCP port"

    def cmd(self, text: str, token: str = TOKEN) -> str:
        return dbcmd.send(text, host="127.0.0.1", port=self.tcp, tok=token, timeout=5)

    def ctl_cmd(self, text: str) -> str:
        return dbcmd.send(text, host="127.0.0.1", port=self.ctl, tok=TOKEN, timeout=5)

    def get(self, path: str, method: str = "GET", body: bytes | None = None, headers: dict | None = None, timeout: float = 10):
        c = http.client.HTTPConnection("127.0.0.1", self.http, timeout=timeout)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, dict(r.getheaders()), data

    def state_json(self) -> dict:
        return json.loads(self.state.read_text())

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


@pytest.fixture(scope="session")
def sink():
    s = WebhookSink()
    yield s
    s.close()


@pytest.fixture(scope="module")
def sim(tmp_path_factory, sink):
    """One simulator per test module, real-time clock, booted and past its boot report."""
    s = Sim(tmp_path_factory.mktemp("sim"), sink)
    sink.wait_for("boot", 10, since=s.start_mark)
    yield s
    s.stop()


@pytest.fixture
def fast_sim(tmp_path, sink):
    """A fresh simulator at 20x time for sleep / reboot / OTA cycles."""
    s = Sim(tmp_path, sink, speed=20.0)
    sink.wait_for("boot", 10, since=s.start_mark)
    yield s
    s.stop()
