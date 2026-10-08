#!/usr/bin/env python3
# ProsperoTV - One scripted run of the test title on a console: install, launch, collect evidence.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs a test script in the app on a console and brings the evidence back.

usage: tools/console-run.py <host> <app folder> <results dir> <script file>

Steps: check the console's services; refuse if the title is running; upload
the app folder (every file verified by hash); put the request beside it; wait
for the title's registration; record the kernel log; launch the title; wait
for the script's report; download the report, the pictures and the app log;
wait for the app to close itself (when the script ends with "quit"); remove
the request.

The script file holds the steps the app understands (ps5/src/tv_dev.hpp). With
filesystem access the app writes to /data/prosperotv/logs, which a PC can
always read. Without it everything is in the title's own storage, which a PC
reads only while the title runs, so a script should still end with
"quit <seconds>" long enough for the download (40 s is plenty).

It never kills the app and never retries. The only things it deletes on the
console are its own request file and upload leftovers. If something is wrong
it stops and says what it saw.

Environment:
  FTP_PORT / KLOG_PORT / ELF_PORT   default 2121 / 3232 / 9021
  PS5_PROTOCOL      path of ps5-homebrew-dev-protocol (default ~/ps5-homebrew-dev-protocol)
  PS5_PAYLOAD_SDK   default ~/prosperotv-ui-build/.deps/native/ps5-payload-sdk
  RUN_TIMEOUT       seconds to wait for the report (default 600)
  DEV_SWITCHES      comma-separated files to put beside the request for this run
  SETTLE_SECONDS    pause before the installed files are verified again (default 0: skipped)
  UPDATED_APP       a built app folder the app is expected to update itself to during the run
                    (with DEV_SWITCHES=update-offer.txt=<file>): once the app has closed, the
                    installed files must be that folder's
"""

import hashlib
import io
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from ftplib import FTP, all_errors, error_perm
from pathlib import Path

BAD_LOG_WORDS = ("fatal", "assertion")
BAD_KLOG_WORDS = ("panic", "crash", "coredump", "segv", "sigsegv")


def say(text):
    print(f"==> {text}", flush=True)


def stop(text):
    raise SystemExit(f"STOPPED: {text}")


def port_open(host, port):
    try:
        with socket.create_connection((host, port), timeout=5):
            return True
    except OSError:
        return False


class Console:
    """One FTP session that reads signed files as the bytes that are stored.

    The console's FTP server has a "SELF" switch that hands out the decrypted
    ELF instead. The command toggles it and the default differs between
    servers, so the switch is flipped until the server says it is off.
    """

    def __init__(self, host, port):
        self.ftp = FTP()
        self.ftp.connect(host, port, timeout=30)
        self.ftp.login("anonymous", "prosperotv")
        self.ftp.voidcmd("TYPE I")
        for _ in range(2):
            try:
                reply = self.ftp.sendcmd("SELF").lower()
            except all_errors:
                break  # a server without the switch
            if "disabled" in reply:
                break

    def close(self):
        try:
            self.ftp.quit()
        except all_errors:
            pass

    def entries(self, path):
        # The server lists only the current directory: change into it first.
        try:
            self.ftp.cwd(path)
        except all_errors:
            return None
        try:
            return {name: facts for name, facts in self.ftp.mlsd() if name not in (".", "..")}
        except all_errors:
            return None
        finally:
            try:
                self.ftp.cwd("/")
            except all_errors:
                pass

    def read(self, path):
        data = io.BytesIO()
        try:
            self.ftp.retrbinary(f"RETR {path}", data.write, blocksize=256 * 1024)
        except all_errors:
            return None
        return data.getvalue()

    def make_dirs(self, path):
        current = ""
        for part in path.strip("/").split("/"):
            current += "/" + part
            try:
                self.ftp.mkd(current)
            except error_perm:
                pass

    def write(self, path, data):
        """Upload under a temporary name, verify by hash, then rename."""
        directory, name = path.rsplit("/", 1)
        temporary = f"{directory}/.{name}.upload"
        self.make_dirs(directory)
        try:
            self.ftp.sendcmd(f"DELE {temporary}")
        except all_errors:
            pass
        self.ftp.storbinary(f"STOR {temporary}", io.BytesIO(data), blocksize=256 * 1024)
        back = self.read(temporary)
        if back is None or hashlib.sha256(back).digest() != hashlib.sha256(data).digest():
            stop(f"upload of {path} did not read back identically")
        try:
            self.ftp.sendcmd(f"DELE {path}")  # only the file this upload replaces
        except all_errors:
            pass
        self.ftp.rename(temporary, path)


def running_titles(console):
    names = console.entries("/mnt/sandbox") or {}
    return sorted({name.split("_")[0] for name in names if name.startswith("PPSA")})


def record_klog(host, port, path, done):
    try:
        with socket.create_connection((host, port), timeout=5) as link, open(path, "wb") as out:
            link.settimeout(1.0)
            while not done.is_set():
                try:
                    chunk = link.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                out.write(chunk)
                out.flush()
    except OSError as error:
        Path(path).write_text(f"kernel log not recorded: {error}\n")


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    host = sys.argv[1]
    package = Path(sys.argv[2])
    results = Path(sys.argv[3])
    script = Path(sys.argv[4]).read_text()
    ftp_port = int(os.environ.get("FTP_PORT", "2121"))
    klog_port = int(os.environ.get("KLOG_PORT", "3232"))
    elf_port = int(os.environ.get("ELF_PORT", "9021"))
    timeout = int(os.environ.get("RUN_TIMEOUT", "600"))
    settle = int(os.environ.get("SETTLE_SECONDS", "0"))
    switches = [name for name in os.environ.get("DEV_SWITCHES", "").split(",") if name]
    updated_app = os.environ.get("UPDATED_APP", "")
    home = Path.home()
    protocol = Path(os.environ.get("PS5_PROTOCOL", home / "ps5-homebrew-dev-protocol"))
    sdk = os.environ.get("PS5_PAYLOAD_SDK",
                         str(home / "prosperotv-ui-build/.deps/native/ps5-payload-sdk"))
    sender = protocol / "scripts/send-controller.sh"
    if not (package / "eboot.bin").is_file():
        stop(f"{package} is not a built app folder")
    title = json.loads((package / "sce_sys/param.json").read_text())["titleId"]
    if not sender.is_file():
        stop(f"launch helper not found: {sender} (set PS5_PROTOCOL)")
    results.mkdir(parents=True, exist_ok=True)
    quits = any(line.split()[:1] == ["quit"] for line in script.splitlines())

    say(f"Checking the console's services at {host}")
    if not port_open(host, ftp_port) or not port_open(host, elf_port):
        stop("FTP or the ELF loader does not answer")
    console = Console(host, ftp_port)
    active = running_titles(console)
    if title in active:
        stop(f"{title} is running on the console; close it first")
    if active:
        say(f"Other titles running (left alone): {', '.join(active)}")

    remote = f"/data/homebrew/{title}"
    first_install = console.entries(remote) is None
    files = sorted(p for p in package.rglob("*") if p.is_file())
    # The executable and the metadata go last: the title is complete when they land.
    files.sort(key=lambda p: (p.name in ("eboot.bin", "param.json"), str(p)))
    uploaded = 0
    manifest = {}
    for path in files:
        relative = path.relative_to(package).as_posix()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        manifest[relative] = digest
        installed = console.read(f"{remote}/{relative}")
        if installed is not None and hashlib.sha256(installed).hexdigest() == digest:
            continue
        console.write(f"{remote}/{relative}", data)
        uploaded += 1
    say(f"Installed {title}: {len(files)} files verified, {uploaded} uploaded")
    (results / "installed.sha256").write_text(
        "".join(f"{digest}  {name}\n" for name, digest in sorted(manifest.items())))

    # ShadowMount Plus copies param.json once, at registration: a title that is
    # already registered would start with the old one.
    staged = console.read(f"/user/app/{title}/sce_sys/param.json")
    if staged is not None and hashlib.sha256(staged).hexdigest() != manifest["sce_sys/param.json"]:
        stop("the console's registered copy of param.json differs from this build's; "
             "nothing was launched")

    # The request goes into the install folder, which the app can always read.
    # The token makes the app honour it once.
    token = str(int(time.time()))
    console.write(f"{remote}/dev/request.txt", f"run {token}\n{script}".encode())
    for name in switches:
        # "name=local file" sends that file; a bare name is a switch.
        name, _, source = name.partition("=")
        console.write(f"{remote}/dev/{name}",
                      Path(source).read_bytes() if source else b"asked for by tools/console-run.py\n")
    console.close()

    waited = 0
    while True:
        console = Console(host, ftp_port)
        registered = "mount.lnk" in (console.entries(f"/user/app/{title}") or {})
        console.close()
        if registered:
            break
        if waited >= 120:
            stop(f"{title} was not registered after 120 s (is ShadowMount Plus running?); "
                 "nothing was launched")
        if waited == 0:
            say("Waiting for the console to register the title")
        time.sleep(5)
        waited += 5
    if first_install or uploaded:
        time.sleep(15)  # let the registration settle before the launch

    done = threading.Event()
    klog = None
    if port_open(host, klog_port):
        klog = threading.Thread(target=record_klog,
                                args=(host, klog_port, results / "klog.txt", done), daemon=True)
        klog.start()
        time.sleep(1.0)

    say(f"Launching {title}")
    started = time.time()
    launch = subprocess.run(["bash", str(sender), "launch", title, host, str(elf_port)],
                            env={**os.environ, "PS5_PAYLOAD_SDK": sdk}, capture_output=True,
                            text=True, check=False)
    if launch.returncode != 0:
        done.set()
        stop(f"the launch helper failed: {launch.stderr.strip() or launch.stdout.strip()}")

    def connect():
        try:
            return Console(host, ftp_port)
        except all_errors:
            done.set()
            stop("the console stopped answering during the run; do not retry, look at klog.txt")

    # With filesystem access the app writes under /data/prosperotv; without
    # it, in its own storage, which is only there while it runs.
    storage = f"/mnt/sandbox/{title}_000/download0"
    places = ["/data/prosperotv/logs/dev", f"{storage}/prosperotv/dev"]
    dev = None
    while time.time() - started < 120 and dev is None:
        time.sleep(3)
        console = connect()
        for place in places:
            if (console.read(f"{place}/handled.txt") or b"").decode().strip() == token:
                dev = place
        console.close()
    if dev is None:
        done.set()
        stop("the app did not take the request (it may be running: close it by hand)")
    say(f"The app is up and running the script (output in {dev})")

    report = None
    while time.time() - started < timeout:
        time.sleep(5)
        console = connect()
        text = (console.read(f"{dev}/report.txt") or b"").decode("utf-8", "replace")
        alive = title in running_titles(console)
        console.close()
        if text.startswith(f"finished {token}"):
            report = text
            break
        if not alive:
            break
    say(f"{'Script finished' if report else 'NO REPORT'} after {time.time() - started:.0f} s; "
        "collecting")

    console = connect()
    if report:
        (results / "report.txt").write_text(report)
    elevated = dev.startswith("/data/")
    logs = "/data/prosperotv/logs" if elevated else f"{storage}/prosperotv"
    log = console.read(f"{logs}/app.log") or b""
    (results / "app.log").write_bytes(log)
    trace = console.read(f"{logs}/debug-trace.txt")
    if trace:
        (results / "debug-trace.txt").write_bytes(trace)
    for name in ("iptv-last-receipt.txt", "iptv-attempt-receipt.txt"):
        receipt = console.read(f"{logs if elevated else storage}/{name}")
        if receipt:
            (results / name).write_bytes(receipt)
    # Only what this run made: the app's storage keeps earlier runs' files.
    pictures = sorted(set(re.findall(r"shot (\S+\.bmp) saved", report or "")))
    played = len(re.findall(r" played ", report or ""))
    pictures += [f"receipt-{number:02d}.txt" for number in range(1, played + 1)]
    # Pictures of the tuning screen, taken by the player as it showed them.
    pictures += sorted(name for name in (console.entries(dev) or {})
                       if name.startswith("tuning-") and name.endswith(".bmp"))
    for name in pictures:
        picture = console.read(f"{dev}/{name}")
        if picture:
            (results / name).write_bytes(picture)
    lifecycle = console.read("/data/shadowmount/debug.log") or b""
    (results / "shadowmount.txt").write_text("".join(
        line + "\n" for line in lifecycle.decode("utf-8", "replace").splitlines() if title in line))
    console.close()
    say(f"Downloaded the log and {len(pictures)} pictures to {results}")

    closed = False
    if quits or not report or updated_app:
        say("Waiting for the app to close itself")
        for _ in range(24):
            console = connect()
            closed = title not in running_titles(console)
            console.close()
            if closed:
                break
            time.sleep(5)
    time.sleep(3)
    done.set()
    if klog is not None:
        klog.join(timeout=5)

    # The request has been honoured; the install folder goes back to what was built.
    console = connect()
    for name in ["request.txt", *(switch.partition("=")[0] for switch in switches)]:
        try:
            console.ftp.sendcmd(f"DELE {remote}/dev/{name}")
        except all_errors:
            pass
    try:
        console.ftp.sendcmd(f"RMD {remote}/dev")
    except all_errors:
        pass
    console.close()

    try:
        from PIL import Image
        for bmp in sorted(results.glob("*.bmp")):
            Image.open(bmp).save(bmp.with_suffix(".png"))
            bmp.unlink()
    except ImportError:
        pass

    problems = []
    if not report:
        problems.append("the script did not report (the app closed or the time ran out)")
    else:
        problems += [f"report: {line}" for line in report.splitlines()
                     if "NOT REACHED" in line or "FAILED" in line or "not understood" in line]
    text = log.decode("utf-8", "replace")
    for line in text.splitlines():
        ours = line.startswith(("[ProsperoTV]", "[TV]"))
        if any(word in line.lower() for word in BAD_LOG_WORDS) or (ours and "failed" in line):
            problems.append(f"app.log: {line.strip()[:200]}")
    if "first-swap ok" not in text:
        problems.append("app.log: no 'first-swap ok' line")
    if quits and not closed:
        problems.append("the app did not close itself after 'quit'")
    if updated_app:
        # The helper replaces the files once the app has gone.
        if not closed:
            problems.append("the app did not close itself for the update")
        expected = {p.relative_to(updated_app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(Path(updated_app).rglob("*")) if p.is_file()}
        different = sorted(expected)
        for _ in range(12):
            time.sleep(5)
            console = connect()
            different = [name for name, digest in expected.items()
                         if hashlib.sha256(console.read(f"{remote}/{name}") or b"").hexdigest() != digest]
            staging = console.entries(f"/data/self-update/{title}")
            console.close()
            if not different and staging is None:
                break
        say(f"After the update: {len(expected) - len(different)} of {len(expected)} files are the "
            f"new version's; staging folder {'still there' if staging is not None else 'gone'}")
        problems += [f"not updated: {name}" for name in different[:8]]
        if staging is not None:
            problems.append("the update's staging folder was left behind")
        manifest = expected if not different else manifest
    klog_file = results / "klog.txt"
    if klog_file.is_file():
        for line in klog_file.read_text(errors="replace").splitlines():
            if title in line and any(word in line.lower() for word in BAD_KLOG_WORDS):
                problems.append(f"klog: {line.strip()[:200]}")
    if report:
        print(report.strip())

    if settle > 0:
        say(f"Waiting {settle} s, then verifying the installed files once more")
        time.sleep(settle)
        console = Console(host, ftp_port)
        for relative, digest in manifest.items():
            data = console.read(f"{remote}/{relative}")
            if data is None or hashlib.sha256(data).hexdigest() != digest:
                problems.append(f"installed file changed or unreadable: {relative}")
        console.close()
    if not all(port_open(host, port) for port in (ftp_port, elf_port)):
        problems.append("the console's services do not all answer after the run")

    if problems:
        print("\n".join(problems))
        stop(f"{len(problems)} problem(s); see {results}")
    say("PASS: the script ran to its end, the log is clean, the console answers")


if __name__ == "__main__":
    main()
