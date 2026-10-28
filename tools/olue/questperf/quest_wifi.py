"""adb over Wi-Fi for the Quest (what SideQuest does): plug in by USB once, then play untethered.

  python quest_wifi.py setup      (USB plugged in) switch adb to network mode and connect over Wi-Fi
  python quest_wifi.py connect    reconnect to the saved address (after the headset slept or the PC's adb restarted)
  python quest_wifi.py status

Network mode lasts until the headset reboots; after a reboot run `setup` again with the cable.  The headset still
asks to allow the PC the first time, as with USB.  The address is kept in tools/olue/out/questperf/quest_ip.txt.
"""
import os
import re
import subprocess
import sys
import time

from questperf import ADB              # (OL_ADB, else ANDROID_HOME's platform-tools, else the PATH)
PORT = 5555
IP_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "questperf", "quest_ip.txt")


def adb(*args):
    r = subprocess.run([ADB] + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace")
    return ((r.stdout or "") + (r.stderr or "")).strip()


def devices():
    """(serial, state) for every device adb sees."""
    out = adb("devices").splitlines()[1:]
    return [tuple(l.split()[:2]) for l in out if len(l.split()) >= 2]


def usb_serial():
    return next((s for s, st in devices() if st == "device" and ":" not in s), None)


def setup():
    serial = usb_serial()
    if not serial:
        sys.exit("plug the headset in by USB first (and allow USB debugging in the headset)")
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", adb("-s", serial, "shell", "ip -f inet addr show wlan0"))
    if not m:
        sys.exit("the headset has no Wi-Fi address: is it on Wi-Fi?")
    ip = m.group(1)
    print("headset %s on Wi-Fi at %s; switching adb to network mode ..." % (serial, ip))
    print(adb("-s", serial, "tcpip", str(PORT)))
    time.sleep(3)
    os.makedirs(os.path.dirname(IP_FILE), exist_ok=True)
    open(IP_FILE, "w").write(ip)
    connect()
    print("\nyou can unplug the cable now; this lasts until the headset reboots.")


def connect():
    if not os.path.exists(IP_FILE):
        sys.exit("no saved address: run `quest_wifi.py setup` with the cable in first")
    ip = open(IP_FILE).read().strip()
    target = "%s:%d" % (ip, PORT)
    for attempt in range(5):
        out = adb("connect", target)
        if "connected to" in out and any(s == target and st == "device" for s, st in devices()):
            print("connected:", target)
            return
        if "unauthorized" in out or any(s == target and st == "unauthorized" for s, st in devices()):
            print("the headset is asking to allow this PC: put it on and press Allow ...")
        time.sleep(2)
    sys.exit("could not connect to %s (%s). Same Wi-Fi network? Headset rebooted? Then `setup` again with the cable." % (target, out))


def status():
    ds = devices()
    if not ds:
        print("no devices")
    for s, st in ds:
        print("%-24s %-12s %s" % (s, st, "Wi-Fi" if ":" in s else "USB"))
    if os.path.exists(IP_FILE):
        print("saved Wi-Fi address:", open(IP_FILE).read().strip())


if __name__ == "__main__":
    {"setup": setup, "connect": connect, "status": status}.get(sys.argv[1] if len(sys.argv) > 1 else "", lambda: sys.exit(__doc__))()
