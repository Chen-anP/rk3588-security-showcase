#!/usr/bin/env python3
"""Local broker acceptance with synthetic events. Run from any directory."""
import json
import os
from pathlib import Path
import queue
import secrets
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def verify(directory, children):
    def spawn(*args, **kwargs):
        child = subprocess.Popen(args, stderr=subprocess.DEVNULL, **kwargs)
        children.append(child)
        return child

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    password = secrets.token_hex(16)
    cert = directory / "cert.pem"
    key = directory / "key.pem"
    credentials = directory / "credentials"
    wrong_credentials = directory / "wrong"
    wrong_ca = directory / "wrong-ca.pem"
    for name, value in [(credentials, password), (wrong_credentials, secrets.token_hex(16))]:
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as file:
            file.write(f"probe:{value}\n")
    for target, private_key in [(cert, key), (wrong_ca, directory / "other-key.pem")]:
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(private_key), "-out", str(target))
    password_file = directory / "broker-password"
    run("mosquitto_passwd", "-b", "-c", str(password_file), "probe", password)
    broker_config = directory / "broker.conf"
    # Root is only used in the isolated WSL test environment. All files are temporary.
    broker_config.write_text(
        f"{'user root' + chr(10) if os.geteuid() == 0 else ''}"
        f"listener {port} 127.0.0.1\nallow_anonymous false\n"
        f"certfile {cert}\nkeyfile {key}\npassword_file {password_file}\n"
        "tls_version tlsv1.2\n")
    probe = directory / "probe"
    run("g++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"),
        *(str(ROOT / path) for path in ("tests/mqtt_probe.cpp", "src/stream/mqtt_publisher.cpp",
                                        "src/stream/credentials.cpp", "src/core/log.cpp")),
        "-lmosquitto", "-o", str(probe))

    def start_broker():
        broker = spawn("mosquitto", "-c", str(broker_config), stdout=subprocess.DEVNULL)
        for _ in range(100):
            if broker.poll() is not None:
                raise RuntimeError("test broker failed to start")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    return broker
            except OSError:
                time.sleep(0.05)
        raise TimeoutError("test broker did not listen")

    def watch():
        sub = spawn("stdbuf", "-oL", "mosquitto_sub", "-d", "-h", "localhost", "-p", str(port),
                    "--cafile", str(cert), "-u", "probe", "-P", password,
                    "-q", "1", "-t", "rksec/probe/#", "-F", "%t %p",
                    text=True, stdout=subprocess.PIPE)
        messages = queue.Queue()

        def read():
            for line in sub.stdout:
                messages.put(line.strip())

        threading.Thread(target=read, daemon=True).start()
        until(messages, lambda line: "Subscribed (mid:" in line)
        return sub, messages

    def client(host="localhost", auth=credentials, ca=cert):
        return spawn(str(probe), host, str(port), str(auth), str(ca),
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def command(process, text):
        process.stdin.write(text + "\n")
        process.stdin.flush()

    def message(messages, topic, payload):
        until(messages, lambda line: line == f"rksec/probe/{topic} {payload}")

    broker = start_broker()
    sub, messages = watch()
    for name, kwargs in [("wrong password", {"auth": wrong_credentials}),
                         ("wrong CA", {"ca": wrong_ca}),
                         ("wrong hostname", {"host": "127.0.0.1"})]:
        bad = client(**kwargs)
        try:
            until(messages, lambda line: line.startswith("rksec/probe/"), timeout=2.2)
        except queue.Empty:
            assert bad.poll() is None, f"{name} probe exited unexpectedly"
        else:
            raise AssertionError(f"{name} published a message")
        command(bad, "stop")
        assert bad.wait(timeout=8) == 0
        print(f"PASS rejected {name}", flush=True)

    publisher = client()
    message(messages, "availability", '{"online":true}')
    command(publisher, "active")
    message(messages, "pir", '{"state":"active"}')
    print("PASS TLS, authentication, online and PIR", flush=True)
    publisher.kill()
    publisher.wait(timeout=5)
    message(messages, "availability", '{"online":false}')
    print("PASS last will on abnormal exit", flush=True)

    publisher = client()
    message(messages, "availability", '{"online":true}')
    broker.terminate()
    broker.wait(timeout=5)
    sub.terminate()
    sub.wait(timeout=5)
    command(publisher, "active")
    command(publisher, "idle")
    command(publisher, "burst")
    result = queue.Queue()
    threading.Thread(target=lambda: result.put(publisher.stdout.readline()), daemon=True).start()
    assert result.get(timeout=5).strip() == "12"
    publisher.send_signal(signal.SIGSTOP)
    start_broker()
    _, messages = watch()
    publisher.send_signal(signal.SIGCONT)
    message(messages, "availability", '{"online":true}')
    message(messages, "pir", '{"state":"idle"}')
    received = []
    for _ in range(128):
        line = until(messages, lambda value: value.startswith("rksec/probe/passage "), timeout=20)
        event = json.loads(line.split(" ", 1)[1])
        assert set(event) == {"event_id", "occurred_ms", "result"}
        assert event["result"] == "simulated_grant"
        received.append(event["event_id"])
    assert received == [f"synthetic-{i}" for i in range(12, 140)], received
    command(publisher, "stop")
    assert publisher.wait(timeout=8) == 0
    message(messages, "availability", '{"online":false}')
    print("PASS reconnect, latest PIR, 128 queued events and graceful offline", flush=True)


def until(messages, predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise queue.Empty
        value = messages.get(timeout=remaining)
        if predicate(value):
            return value


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rksec-mqtt-") as directory:
        processes = []
        try:
            verify(Path(directory), processes)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
