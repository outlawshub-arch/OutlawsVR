"""questperf auto (REQ-QUESTPERFRUN, user 28 Oct 2026): one level of the story recorded on the Quest by itself, every time
it is played, to compare runs (the same level at the top graphics settings, then at lower ones).

  python questperf.py auto [--level N | --map NAME] [--hz N]   armed until stopped: each start of that level (default the
                                                      story's 2nd, The Town) is a new capture, and its end stops it; the
                                                      numbers are against N Hz (default: the saved Refresh Rate row), and
                                                      a capture whose compositor ran another rate says so at its top
  python questperf.py auto stop                       stop an armed session (a capture under way is finished first)
  python questperf.py compare [DIR]                   (re)write compare.txt over an auto session (default: the newest)
  python questperf.py auto --fake FEED.json           the same against a scripted feed (questperf_fake.py), no headset

A capture starts when the level's build is done and play begins (the game log's "OLRT level load: <map>", not the main
menu's) and stops at the level's end: complete, Restart Level, a load, another level, the menu, the game closed, or the
app paused (the headset off) for more than a minute.  A death and its respawn stay in the same capture, with a marker.
Each capture: Saved/Perf/capture.on (the game's row a frame, Shipping too), the GPU counters (ovrgpuprofiler), the
compositor's lines (VrApi), settings.txt at the start (and again when a Graphics row changes), then report.txt.  Only
live play counts in the numbers (the capture's in_play column: no menu, pause, death view, movie or load).
Sessions go to tools/olue/out/questperf/auto_<date_time>/capture_<n>_<map>_<time>/, compare.txt beside them.
In Shipping the game's log is a file (Saved/Logs/OutlawsUE.log, written a line at a time), not logcat: that file is
followed; logcat gives the compositor's lines.
"""
import datetime
import glob
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

STORY = ["hideout", "townex", "train", "canyon", "mill", "simms", "miner", "cliff", "ranch"]
STORY_NAMES = {"hideout": "The Hideout", "townex": "The Town", "train": "The Train", "canyon": "The Canyon", "mill": "The Sawmill",
               "simms": "Simm's Fort", "miner": "The Mine", "cliff": "The Cliff Dwellings", "ranch": "Bob's Ranch"}
# the randomizer's run order names the HD build of a level (OUTLAWS.RCS LEVELHD), the log's level loads the level's own
ALIASES = {"town": "townex"}
GRAPHICS_ROWS = [("Remastered Graphics", "RemasteredGraphics"), ("Tinting", "Tinting"), ("Stochastic Textures", "StochasticTextures"),
                 ("Brightness", "Gamma"), ("Resolution", "RenderScale"), ("Refresh Rate", "RefreshRate"), ("Lamp Lights", "LampLights"),
                 ("Object Detail", "ObjectDetail"), ("Quest 2 Mode", "Quest2Mode"), ("Performance HUD", "PerfHud"), ("Grab Glow", "GrabGlow")]
GAMEPLAY_ROWS = [("Bad guys (single player)", "SoloBadGuys"), ("Randomizer: bad guys", "RandoEnemies"), ("Randomizer: items", "RandoItems"),
                 ("Randomizer: mission order", "RandoOrder"), ("Randomizer: bosses", "RandoBosses"), ("Randomizer: critters", "RandoCritters"),
                 ("Randomizer: seed", "RandoSeed")]
CHOICES = {"RandoEnemies": ["off", "same strength", "anything"], "RandoItems": ["off", "shuffle places", "re-roll"],
           "RandoOrder": ["off", "shuffle middle", "shuffle all"], "RandoBosses": ["off", "in the mix"], "RandoCritters": ["off", "on"],
           "ObjectDetail": ["Near", "Normal", "Far"]}
PAUSE_LIMIT = 60.0          # the app paused (no compositor line from the game) this long ends a capture
SETTINGS_POLL = 10.0
LINE = re.compile(r"^\[([\d.:\-]+)\]\[\s*(\d+)\](\w+): (?:(?:Display|Warning|Error|Fatal|Verbose): )?(.*)$")
VRAPI = re.compile(r"^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+) \w/VrApi\s*\(\s*(\d+)\): (FPS=.*)$")


def norm(m):
    m = (m or "").strip().lower()
    return ALIASES.get(m, m)


def level_label(m):
    return "%s (%s)" % (STORY_NAMES.get(norm(m), m), m) if norm(m) in STORY_NAMES else m


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pct(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(p / 100.0 * len(s)))]


def avg(vals):
    return statistics.fmean(vals) if vals else 0.0


def vr_fields(line):
    """One VrApi line's numbers: FPS=72/72,...,Stale=0,...,CPU4/GPU=2/2,2208/456MHz,...,Free=954MB,...,Temp=42.0C/0.0C,...,GPU%=0.68,CPU%=0.57"""
    f = {}
    m = re.search(r"FPS=(\d+)/(\d+)", line)
    if m:
        f["fps"], f["hz"] = int(m.group(1)), int(m.group(2))
    for k, pat in (("stale", r"Stale=(\d+)"), ("free_mb", r"Free=(\d+)MB"), ("temp", r"Temp=([\d.]+)C"), ("gpu_pct", r"GPU%=([\d.]+)"),
                   ("cpu_pct", r"CPU%=([\d.]+)"), ("app_ms", r"App=([\d.]+)ms")):
        m = re.search(pat, line)
        if m:
            f[k] = float(m.group(1))
    m = re.search(r"CPU(\d+)/GPU=(\d+)/(\d+),(\d+)/(\d+)MHz", line)
    if m:
        f["levels"] = "CPU %s, GPU %s (clocks %s / %s MHz)" % (m.group(2), m.group(3), m.group(4), m.group(5))
    return f


# ------------------------------------------------------------------------------------------------ the headset

def parse_log_state(out):
    """`echo P:$(pidof ..); echo S:$(stat -c %s ..); head -c 40 ..` -> (pid or "", size or -1, the head); None when adb did
    not reach the headset at all (Wi-Fi dropped: the headset asleep)."""
    if "P:" not in out:
        return None
    pid = re.search(r"^P:\s*(\d*)", out, re.M)
    size = re.search(r"^S:\s*(\d*)", out, re.M)
    head = out[size.end():].strip() if size else ""
    return (pid.group(1) if pid else ""), (int(size.group(1)) if size and size.group(1) else -1), head


class Quest:
    """The real headset over adb (questperf's chosen device: ANDROID_SERIAL, a Quest only)."""
    def __init__(self, qp):
        self.qp = qp
        self.log_path = qp.SAVED + "/Logs/OutlawsUE.log"
        self.lc = None

    def now(self):
        return time.time()

    def sleep(self, s):
        time.sleep(s)

    def tick(self):
        time.sleep(1.0)

    def raw(self, *args):
        return subprocess.run([self.qp.ADB] + list(args), capture_output=True).stdout

    def log_state(self):
        """(pid, size, the file's first bytes): a new run moves the old log aside and starts a new file."""
        return parse_log_state(self.qp.adb("shell", "echo P:$(pidof %s); echo S:$(stat -c %%s %s 2>/dev/null); head -c 40 %s 2>/dev/null" % (
            self.qp.PKG, self.log_path, self.log_path)))

    def read_log(self, offset):
        return self.raw("exec-out", "tail -c +%d %s" % (offset + 1, self.log_path))

    def reconnect(self):
        serial = os.environ.get("ANDROID_SERIAL", "")
        if ":" in serial:
            self.qp.adb("connect", serial)

    def settings_text(self):
        b = self.raw("exec-out", "run-as %s cat files/UnrealGame/OutlawsUE/OutlawsUE/Saved/OutlawsSettings.ini" % self.qp.PKG)
        t = b.decode("utf-8", "replace")
        return t if "=" in t and "run-as:" not in t else None

    def version(self):
        out = self.qp.adb("shell", "dumpsys package %s | grep -E 'versionCode|versionName' | head -2" % self.qp.PKG)
        code = re.search(r"versionCode=(\d+)", out)
        name = re.search(r"versionName=(\S+)", out)
        return (code.group(1) if code else None), (name.group(1) if name else None)

    def device(self):
        return {k: self.qp.adb("shell", "getprop " + p) for k, p in (("model", "ro.product.model"), ("make", "ro.product.manufacturer"),
                                                                   ("serial", "ro.serialno"))}

    def battery(self):
        out = self.qp.adb("shell", "dumpsys battery")
        b = {}
        for k in ("level", "temperature", "status", "AC powered", "USB powered"):
            m = re.search(r"^\s*%s:\s*(\S+)" % re.escape(k), out, re.M)
            if m:
                b[k] = m.group(1)
        return b

    def capture_flag(self, on):
        return self.qp.capture_flag(on)

    def captures(self):
        return self.qp.device_captures()

    def pull(self, name, local):
        self.qp.adb("pull", "%s/Perf/%s" % (self.qp.SAVED, name), local)
        return os.path.exists(local)

    def start_gpu(self, path):
        return self.qp.spawn([sys.executable, os.path.join(self.qp.HERE, "questperf.py"), "_gpu", path], path + ".log")

    def stop_gpu(self, pid):
        if pid:
            self.qp.kill(pid)

    def start_logcat(self, sink):
        """The compositor's once-a-second line (VrApi) and the app's lifecycle, stamped with this PC's clock as they come;
        started again if the stream drops (Wi-Fi)."""
        def run():
            while True:
                p = subprocess.Popen([self.qp.ADB, "logcat", "-v", "time", "-T", "1", "VrApi:I", "ActivityManager:I", "*:S"],
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
                self.lc = p
                for line in p.stdout:
                    sink(time.time(), line.rstrip("\r\n"))
                time.sleep(3)
        threading.Thread(target=run, daemon=True).start()

    def stop_logcat(self):
        if self.lc:
            self.lc.kill()

    def finished(self):
        return False


# ------------------------------------------------------------------------------------------------ the session

class Capture:
    def __init__(self, n, level, d, t0):
        self.n, self.map, self.dir, self.t0 = n, level, d, t0
        self.end_t = None
        self.reason = None
        self.ending = False
        self.gpu = None
        self.markers = []
        self.device_file = None
        self.captures_before = []
        self.soft = False
        self.kind = "level"
        self.game_settings = None
        self.settings = {}
        self.changes = 0
        self.last_settings_poll = t0
        self.deaths = 0


class Auto:
    def __init__(self, qp, dev, level_n=2, map_name=None, out=None, expect_hz=None):
        self.qp, self.dev = qp, dev
        self.expect_hz = expect_hz
        self.level_n, self.map_arg = level_n, norm(map_name) if map_name else None
        stamp = datetime.datetime.fromtimestamp(dev.now()).strftime("%Y%m%d_%H%M%S")
        self.dir = out or os.path.join(qp.OUT, "auto_" + stamp)
        os.makedirs(self.dir, exist_ok=True)
        self.autolog = open(os.path.join(self.dir, "auto.log"), "a", encoding="utf-8")
        self.gamelog = open(os.path.join(self.dir, "game_log.txt"), "a", encoding="utf-8")
        self.lclog = open(os.path.join(self.dir, "logcat_all.txt"), "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.vr = []                      # (pc time, line) for every compositor/lifecycle line
        self.lines = []                   # (pc time, raw game log line)
        self.offset, self.head, self.rest, self.pid = 0, None, b"", ""
        self.contact, self.lost_at, self.deferred = True, None, []
        self.g = {}                       # what the game said about itself: build, headset, graphics, eye, hd, rando, ...
        self.rando_order = None
        self.learned = {}                 # run position -> map, from the randomizer's lines
        self.last_pos = None              # (map, its position in the run)
        self.cur, self.front, self.died = None, False, False
        self.pending = None
        self.cap = None
        self.done = []
        self.last_fps = dev.now()
        self.last_line = dev.now()
        self.vr_game = False              # a compositor line seen from the game's own process
        self.settings_cache = None

    # ---- output
    def log(self, msg, show=True):
        line = "%s %s" % (datetime.datetime.fromtimestamp(self.dev.now()).strftime("%H:%M:%S"), msg)
        self.autolog.write(line + "\n")
        self.autolog.flush()
        if show:
            print(line, flush=True)

    # ---- which level
    def target(self):
        if self.map_arg:
            return self.map_arg
        if self.rando_order in (None, 0):
            return STORY[self.level_n - 1] if 1 <= self.level_n <= len(STORY) else None
        return self.learned.get(self.level_n)

    def describe_target(self):
        t = self.target()
        if self.map_arg:
            return "the map %s" % level_label(self.map_arg)
        if t:
            return "level %d of the story run, %s" % (self.level_n, level_label(t))
        return ("level %d of the story run (the mission order is shuffled: its map is learnt at level %d's end, or arm with "
                "--map <name>)" % (self.level_n, self.level_n - 1))

    # ---- the compositor's lines (the logcat thread)
    def on_logcat(self, t, line):
        with self.lock:
            self.vr.append((t, line))
            self.lclog.write("%.3f %s\n" % (t, line))
            m = VRAPI.match(line)
            if m and self.pid and m.group(2) == self.pid:
                self.last_fps = t
                self.vr_game = True

    # ---- the game's log
    def poll_log(self, act=True):
        st = self.dev.log_state()
        if st is None:
            # (no answer from the headset: nothing about the game is known; the capture is left as it is)
            now = self.dev.now()
            if self.contact:
                self.contact, self.lost_at = False, now
                self.log("lost contact with the headset (adb over Wi-Fi: asleep or out of reach); trying again")
            if int(now - self.lost_at) % 10 == 0 and hasattr(self.dev, "reconnect"):
                self.dev.reconnect()
            return
        if not self.contact:
            self.contact = True
            self.log("contact again after %.0f s" % (self.dev.now() - self.lost_at))
            self.flush_deferred()
        pid, size, head = st
        if pid != self.pid:
            if self.pid and not pid:
                self.log("the game's process ended (pid %s)" % self.pid)
            elif pid:
                self.log("the game's process: %s" % pid, show=bool(self.pid))
            self.pid = pid
        if size < 0:
            return
        if head != self.head or size < self.offset:
            if self.head is not None:
                self.log("a new game log (the game started again)")
            self.head, self.offset, self.rest = head, 0, b""
        if size > self.offset:
            data = self.dev.read_log(self.offset)
            self.offset += len(data)
            data = self.rest + data
            parts = data.split(b"\n")
            self.rest = parts.pop()
            t = self.dev.now()
            if parts:
                self.last_line = t
            for p in parts:
                raw = p.decode("utf-8", "replace").rstrip("\r").lstrip("\ufeff")
                self.lines.append((t, raw))
                self.gamelog.write(raw + "\n")
                self.on_line(raw, t, act)
            self.gamelog.flush()

    def on_line(self, raw, t, act):
        m = LINE.match(raw)
        if not m:
            return
        frame, text = int(m.group(2)), m.group(4)
        g = self.g
        x = re.match(r"OutlawsRuntime module loaded, build (\S+)(?: \((.*?)\))?", text)
        if x:
            g["build"] = "%s (%s)" % (x.group(1), x.group(2)) if x.group(2) else x.group(1)
            if act and self.cap and not self.cap.ending:
                self.stop("the game started again (its process ended)", t)
            self.cur, self.front, self.died = None, False, False
        x = re.match(r"OLRT headset: (.*?), profile (\S+) \((.*?)\)", text)
        if x:
            g["headset"], g["profile"] = x.group(1), "%s (%s)" % (x.group(2), x.group(3))
        x = re.match(r"OLRT graphics \((\S+), (.*?)\): resolution (.*?), refresh (.*?), lamp lights (\d+)(.*?), performance HUD (\S+)", text)
        if x:
            was = g.get("graphics")
            g["graphics"] = {"profile": x.group(1), "why": x.group(2), "resolution": x.group(3), "refresh": x.group(4),
                             "lamp lights": x.group(5) + x.group(6), "performance HUD": x.group(7)}
            if act and self.cap and was and {k: v for k, v in was.items() if k != "why"} != {k: v for k, v in g["graphics"].items() if k != "why"}:
                self.settings_changed("the game applied: " + text, t)
        x = re.match(r"OLRT eye buffer: (.*)", text)
        if x:
            g["eye"] = x.group(1)
        x = re.match(r"OLRT graphics: HD textures (on|off)(.*)", text)
        if x:
            g["hd chosen"] = x.group(1) + x.group(2)
            if act and self.cap:
                self.settings_changed("the game applied: " + text, t)
        x = re.match(r"OLRT remastered graphics (on|off)(.*)", text)
        if x:
            g["hd"] = x.group(1) + x.group(2)
        x = re.match(r"OLRT (gamma: .*|graphics: Quest 2 Mode .*)", text)
        if x and act and self.cap:
            self.settings_changed("the game applied: " + text, t)
        x = re.match(r"OLRT single player bad guys: x([\d.]+)", text)
        if x:
            g["bad guys"] = "x" + x.group(1)
            if act and self.cap:
                self.marker("Bad guys changed to x%s (Gameplay)" % x.group(1), t)
        x = re.match(r"OLRANDO (run \(.*?\)|load): enemies (.*?), items (.*?), mission order (.*?), .*?seed (\d+)", text)
        if x:
            g["rando"] = text[len("OLRANDO "):]
            self.rando_order = 0 if x.group(4) == "off" else 1
            if "shuffle middle" in x.group(4):
                self.learned.setdefault(1, STORY[0])          # (the first and the last stay where they are)
                self.learned.setdefault(len(STORY), STORY[-1])
        x = re.match(r"OLRT resume (\d+) \(app resumed\): before: (.*)", text)
        if x:
            g["resume"] = x.group(2)
            if act and self.cap:
                self.marker("the app resumed (resume %s)" % x.group(1), t)
        x = re.match(r"OLRANDO story: (\S+) is level (\d+) of (\d+) in this run: .*?, next (\S+)", text)
        if x:
            k = int(x.group(2))
            self.learned[k] = norm(x.group(1))
            if x.group(4) not in ("the", ""):
                self.learned[k + 1] = norm(x.group(4))
            self.last_pos = (norm(x.group(1)), k)
        x = re.match(r"OLRT level complete \((.*?)\): movie (\S+), next level (\S+)", text)
        if x:
            if self.rando_order and self.last_pos and self.last_pos[0] == norm(self.cur):
                self.learned[self.last_pos[1] + 1] = norm(x.group(3))
            if act and self.cap and not self.cap.ending:
                self.stop("the level's end: complete (%s), next %s" % (x.group(1), x.group(3)), t)
        x = re.match(r"OLRT level load: (\S+) (\d+) ms", text)
        if x:
            self.on_load(norm(x.group(1)), frame, t, act)
            return
        x = re.match(r"OLRT main menu level (\S+):", text)
        if x:
            self.front = True
            if self.pending:
                self.log("the main menu's own view is %s: not a start" % x.group(1), show=act)
                self.pending = None
            if act and self.cap:
                self.resolve("quit to the main menu", t)
            return
        if text.startswith("OLRT level restart"):
            if act and self.cap and not self.cap.ending:
                if self.died:
                    self.marker("respawn after the death: the level builds again (same capture)", t)
                else:
                    self.stop("a level rebuild began", t, soft=True)
            return
        if text.startswith("OLRT player died"):
            if not self.died and act and self.cap:
                self.cap.deaths += 1
                self.marker("death %d" % self.cap.deaths, t)
            self.died = True
            return
        if re.match(r"OLEXIT|OLPERF summary, level \S+ left \(the game ends\)", text):
            if act and self.cap and not self.cap.ending:
                self.stop("the game closed (%s)" % text[:60], t)
            return
        x = re.match(r"OLPERF test start: level (\S+), capture (.+?), settings (.+)$", text)
        if x and act:
            m_, cap_, set_ = norm(x.group(1)), os.path.basename(x.group(2).strip().replace("\\", "/")), os.path.basename(x.group(3).strip().replace("\\", "/"))
            c = self.cap
            if not (c and not c.ending and c.map == m_):
                self.pending = None
                self.start(m_, t, test=True)
                c = self.cap
            c.kind, c.device_file, c.game_settings = "test", cap_, set_
            self.marker("the Performance test run (the game's own capture %s, its settings %s)" % (cap_, set_), t)
            return
        x = re.match(r"OLPERF test death (\d+)", text)
        if x and act and self.cap:
            self.marker("the test run's death %s" % x.group(1), t)
            return
        x = re.match(r"OLPERF test end: (.*?) \((\d+) frames\)", text)
        if x and act and self.cap and not self.cap.ending:
            self.stop("the Performance test ended: %s (the game's %s frames)" % (x.group(1), x.group(2)), t)
            return
        x = re.match(r"OLPERF capture started: (.*)", text)
        if x and self.cap and getattr(self.cap, "kind", "") != "test":
            self.cap.device_file = os.path.basename(x.group(1).strip().replace("\\", "/"))
            self.log("the game is capturing: %s" % self.cap.device_file, show=act)
        if re.match(r"OLPERF capture stopped: .*", text) and act:
            self.log("the game: " + text)
        x = re.match(r"OLPERF mark (\d+) \((.*)\)", text)
        if x and act and self.cap:
            self.marker("perf mark %s (%s)" % (x.group(1), x.group(2)), t)
        if text.startswith("OLRT pause page rows") and act and self.cap:
            self.marker("pause menu open (its frames are left out)", t)
        if self.pending and frame != self.pending["frame"]:
            self.confirm(t, act)

    def on_load(self, m, frame, t, act):
        self.cur, self.front = m, False
        if self.cap:
            if self.cap.ending and m == self.cap.map:
                self.resolve("the level built again (Restart Level or a load; a new capture starts)", t)
            elif self.cap.ending:
                self.cap.reason = "%s: another level (%s)" % (self.cap.reason, m) if self.cap.soft else self.cap.reason
                self.cap.soft = True          # (the main menu's line, in this frame, says it was the menu)
            elif m == self.cap.map and self.died:
                self.marker("respawn: %s built again after the death" % m, t)
                self.died = False
                return
            elif m == self.cap.map:
                self.stop("the level built again", t)
                self.finish()
            else:
                self.stop("another level: %s" % m, t, soft=True)
        self.died = False
        if m == self.target():
            self.pending = {"map": m, "t": t, "frame": frame}
        elif act and self.target() is None and self.level_n:
            self.log("a level loaded: %s (still to learn which map is level %d)" % (m, self.level_n))

    def confirm(self, t, act):
        p, self.pending = self.pending, None
        if not act:
            self.log("%s was already under way when this started: the capture waits for its next start" % level_label(p["map"]))
            return
        self.start(p["map"], t)

    # ---- a capture
    def start(self, m, t, test=False):
        if self.cap:
            self.finish()
        n = len(self.done) + 1
        d = os.path.join(self.dir, "capture_%02d_%s_%s" % (n, m, datetime.datetime.fromtimestamp(t).strftime("%H%M%S")))
        os.makedirs(d, exist_ok=True)
        c = Capture(n, m, d, t)
        c.captures_before = self.dev.captures()
        # (a Performance test run's capture is the game's own: no capture.on, which would keep it on past the test's end)
        ok = True if test else self.dev.capture_flag(True)
        c.gpu = self.dev.start_gpu(os.path.join(d, "gpu_metrics.jsonl"))
        self.cap = c
        self.last_fps = t
        self.marker("start: %s built, play begins%s" % (level_label(m), "" if ok else " (capture.on could NOT be made)"), t)
        text, values, missing = self.settings_dump("at the start")
        c.settings = values
        open(os.path.join(d, "settings.txt"), "w", encoding="utf-8").write(text)
        self.log("CAPTURE %d STARTED: %s; settings.txt written%s" % (n, level_label(m), (", not read: " + ", ".join(missing)) if missing else ""))

    def marker(self, what, t):
        if self.cap:
            self.cap.markers.append((t, what))
            self.log("  capture %d marker: %s" % (self.cap.n, what))

    def settings_changed(self, why, t):
        c = self.cap
        if not c or c.ending:
            return
        text, values, _ = self.settings_dump("after a change (%s)" % why)
        # (a real difference dumps again; the eye buffer and the compositor's rate follow a change by themselves)
        follow = ("Applied: eye buffer", "Applied: refresh Hz", "Applied: HD art", "Applied: vr.PixelDensity")
        diff = [k for k in values if k.startswith(("Graphics", "Applied")) and k not in follow and values.get(k) != c.settings.get(k)]
        if not diff:
            return
        c.changes += 1
        open(os.path.join(c.dir, "settings_change_%d.txt" % c.changes), "w", encoding="utf-8").write(text)
        self.marker("Graphics changed: %s; settings dumped again (settings_change_%d.txt)" % (
            ", ".join("%s %s -> %s" % (k.replace(": ", " ", 1), c.settings.get(k), values.get(k)) for k in diff), c.changes), t)
        c.settings = values

    def poll_settings(self, t):
        c = self.cap
        if not c or c.ending or t - c.last_settings_poll < SETTINGS_POLL:
            return
        c.last_settings_poll = t
        s = self.dev.settings_text()
        if s and s != self.settings_cache:
            was = self.settings_cache
            self.settings_cache = s
            if was is not None:
                self.settings_changed("the settings file changed", t)

    def stop(self, reason, t, soft=False):
        """The level's play ended: the game's capture off at once; the rest (pull, report) once the reason is clear (soft:
        a later line of the same build says what it was, the menu or a level), else in 5 s."""
        c = self.cap
        if not c or c.ending:
            return
        c.ending, c.end_t, c.reason, c.soft = True, t, reason, soft
        self.dev.capture_flag(False)
        self.marker("end: " + reason, t)

    def resolve(self, detail, t):
        c = self.cap
        if not c:
            return
        if not c.ending:
            self.stop(detail, t)
        elif c.soft:
            c.reason = "%s: %s" % (c.reason.split(": another level")[0], detail)
        self.finish()

    def finish(self):
        c, self.cap = self.cap, None
        if not c:
            return
        if not self.contact:
            # (pulled once the headset answers again: its capture.on is removed first, so the game closes the file)
            self.deferred.append(c)
            self.log("CAPTURE %d: the pull waits for contact with the headset" % c.n)
            return
        self.collect(c)

    def flush_deferred(self):
        if not self.deferred:
            return
        if not self.cap:
            self.dev.capture_flag(False)
            self.dev.sleep(3)
        while self.deferred:
            self.collect(self.deferred.pop(0))

    def collect(self, c):
        if not c.ending:
            c.ending, c.end_t = True, self.dev.now()
            self.dev.capture_flag(False)
        self.dev.sleep(3)                       # the game sees capture.on gone within a second and writes its last rows
        self.dev.stop_gpu(c.gpu)
        d = c.dir
        name = c.device_file
        if not name:
            new = [x for x in self.dev.captures() if x not in c.captures_before]
            name = new[-1] if new else None
        got = self.dev.pull(name, os.path.join(d, name)) if name else False
        for f in ("perf_timeline.csv", "perf_counts.csv", "hitches.csv", "performance.txt"):
            self.dev.pull(f, os.path.join(d, f))
        if c.game_settings:
            self.dev.pull(c.game_settings, os.path.join(d, "game_settings.txt"))
        with self.lock:
            vr = [(t, l) for t, l in self.vr if c.t0 - 2 <= t <= c.end_t + 1]
        open(os.path.join(d, "logcat.txt"), "w", encoding="utf-8").write("\n".join(l for _, l in vr) + "\n")
        json.dump([[t, l] for t, l in vr], open(os.path.join(d, "vrapi_times.json"), "w"))
        open(os.path.join(d, "game.log"), "w", encoding="utf-8").write(
            "\n".join(l for t, l in self.lines if c.t0 - 1 <= t <= c.end_t + 4) + "\n")
        open(os.path.join(d, "markers.txt"), "w", encoding="utf-8").write(
            "\n".join("%s  %s" % (datetime.datetime.fromtimestamp(t).strftime("%H:%M:%S"), w) for t, w in c.markers) + "\n")
        meta = {"n": c.n, "map": c.map, "label": level_label(c.map), "t0": c.t0, "t1": c.end_t, "reason": c.reason, "deaths": c.deaths,
                "expect_hz": c.settings.get("expect_hz"), "kind": c.kind, "game_settings": c.game_settings,
                "changes": c.changes, "markers": c.markers, "device_file": name, "settings": c.settings}
        json.dump(meta, open(os.path.join(d, "capture.json"), "w"), indent=1)
        if not got:
            self.log("CAPTURE %d: the game's capture file was NOT pulled (%s)" % (c.n, name or "no file named"))
        s = summarize(d, self.qp)
        head = headline(s)
        report_capture(self.qp, d, s, head)
        self.done.append(d)
        self.log("CAPTURE %d DONE: %s" % (c.n, head))
        self.log("  report: %s" % os.path.join(d, "report.txt"))
        if len(self.done) >= 2:
            ch = compare(self.dir, quiet=True)
            self.log("COMPARE: %s" % ch)

    # ---- settings.txt
    def settings_dump(self, when):
        """The settings and what the game applied, each with where it came from; returns (text, values, unreadable)."""
        v, missing, out = {}, [], []
        w = out.append
        now = self.dev.now()
        w("Settings %s: %s" % (when, datetime.datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M:%S")))
        w("=" * 100)
        g = self.g
        code, vname = self.dev.version()
        dev = self.dev.device()
        w("build (the game log)        %s" % g.get("build", "NOT READ (no module line in this log)"))
        w("versionCode (dumpsys)       %s%s" % (code or "NOT READ", " (versionName %s)" % vname if vname else ""))
        w("headset (the game log)      %s, profile %s" % (g.get("headset", "?"), g.get("profile", "?")))
        w("headset (getprop)           %s %s, serial %s" % (dev.get("make", "?"), dev.get("model", "?"), dev.get("serial", "?")))
        v["build"], v["versionCode"], v["profile"] = g.get("build"), code, g.get("profile")
        for k, val in (("build", g.get("build")), ("versionCode", code), ("headset/profile", g.get("profile"))):
            if not val:
                missing.append(k)
        s = self.dev.settings_text()
        if s:
            self.settings_cache = s
        kv = dict(re.findall(r"^(\w+)=(.*?)\s*$", s or "", re.M))
        src = "OutlawsSettings.ini (the internal Saved, run-as)" if s else "NOT READ: OutlawsSettings.ini (run-as refused)"
        if not s:
            missing.append("the settings file (every saved row)")
        w("\n## Graphics rows, saved (%s; absent = the headset's default)" % src)
        for label, key in GRAPHICS_ROWS:
            val = kv.get(key)
            shown = "(absent: default)" if val is None else val
            if key in CHOICES and val is not None and val.lstrip("-").isdigit() and 0 <= int(val) < len(CHOICES[key]):
                shown = "%s (%s)" % (val, CHOICES[key][int(val)])
            w("  %-22s %-12s %s" % (label, key, shown if s else "NOT READ"))
            v["Graphics: " + label] = val
        gr = g.get("graphics") or {}
        w("\n## Applied (the game log's latest lines)")
        if not gr:
            missing.append("the applied graphics line (OLRT graphics ...)")
        w("  graphics line              %s" % ("profile %s, %s" % (gr.get("profile"), gr.get("why")) if gr else "NOT READ"))
        for k in ("resolution", "refresh", "lamp lights", "performance HUD"):
            w("  %-26s %s" % (k, gr.get(k, "NOT READ")))
            v["Applied: " + k] = gr.get(k)
        pd = re.search(r"vr\.PixelDensity ([\d.]+)", gr.get("resolution", "") + " " + g.get("eye", ""))
        w("  %-26s %s" % ("vr.PixelDensity", pd.group(1) if pd else "NOT READ"))
        v["Applied: vr.PixelDensity"] = pd.group(1) if pd else None
        w("  %-26s %s" % ("eye buffer", g.get("eye", "NOT READ (logged a few seconds after a graphics change)")))
        v["Applied: eye buffer"] = (re.match(r"render target (\d+x\d+)", g.get("eye", "")) or [None, None])[1]
        w("  %-26s %s" % ("HD art (this level)", g.get("hd", "NOT READ (logged at the level's build)")))
        w("  %-26s %s" % ("HD art (chosen)", g.get("hd chosen", "NOT READ")))
        v["Applied: HD art"] = (g.get("hd") or "").split(" ")[0] or None
        for k in ("eye buffer", "HD art"):
            if not v["Applied: " + k]:
                missing.append("applied " + k)
        with self.lock:
            last = next((l for t, l in reversed(self.vr) if VRAPI.match(l) and (not self.pid or VRAPI.match(l).group(2) == self.pid)), None)
        vf = vr_fields(last) if last else {}
        w("  %-26s %s" % ("refresh (compositor)", "%s Hz, delivering %s fps" % (vf["hz"], vf["fps"]) if "hz" in vf else "NOT READ (no VrApi line yet)"))
        v["Applied: refresh Hz"] = vf.get("hz")
        # (REQ-QUESTPERFRUN: both runs at 120 Hz) the rate asked against the rate in effect
        saved_hz = kv.get("RefreshRate")
        expect = self.expect_hz or (int(saved_hz) if saved_hz and saved_hz.isdigit() else None)
        v["expect_hz"] = expect
        asked = re.match(r"(\d+) Hz asked", gr.get("refresh", ""))
        w("\n## Refresh check%s" % (" (expected %d Hz, budget %.2f ms)" % (expect, 1000.0 / expect) if expect else " (no rate expected: the Refresh Rate row is absent)"))
        w("  %-26s %s" % ("saved Refresh Rate", saved_hz or "(absent: the headset's own)"))
        w("  %-26s %s" % ("applied (the game log)", gr.get("refresh", "NOT READ")))
        w("  %-26s %s" % ("in effect (compositor)", "%s Hz" % vf["hz"] if "hz" in vf else "NOT READ (no VrApi line yet)"))
        if expect:
            ok_log = bool(asked) and int(asked.group(1)) == expect
            ok_vr = vf.get("hz") == expect
            v["refresh_ok"] = ok_log and ok_vr
            w("  %-26s %s" % ("verdict", "%d Hz IN EFFECT (asked in the log and the compositor's rate)" % expect if ok_log and ok_vr else
                              "NOT IN EFFECT: the log asked %s, the compositor runs %s" % (asked.group(1) + " Hz" if asked else "?", "%s Hz" % vf.get("hz") if vf.get("hz") else "?")))
            if not (ok_log and ok_vr):
                missing.append("%d Hz not confirmed" % expect)
        w("\n## Gameplay rows, saved (%s)" % src)
        for label, key in GAMEPLAY_ROWS:
            val = kv.get(key)
            shown = "(absent: default)" if val is None else (val if val != "" else "(empty: a random seed each game)")
            if key in CHOICES and val and val.isdigit() and int(val) < len(CHOICES[key]):
                shown = "%s (%s)" % (val, CHOICES[key][int(val)])
            if key == "SoloBadGuys" and val:
                shown = "x%s" % val
            w("  %-26s %-14s %s" % (label, key, shown if s else "NOT READ"))
            v["Gameplay: " + label] = val
        w("  %-26s %s" % ("bad guys (the log)", g.get("bad guys", "(no change logged this run)")))
        w("  %-26s %s" % ("randomizer run (the log)", g.get("rando", "NOT READ (no OLRANDO run/load line: no game started yet)")))
        w("\n## The headset's state")
        w("  %-26s %s" % ("CPU and GPU levels", vf.get("levels", "NOT READ (no VrApi line yet)")))
        if "levels" not in vf:
            missing.append("CPU and GPU levels")
        w("  %-26s %s" % ("compositor temperature", "%.1f C" % vf["temp"] if "temp" in vf else "NOT READ"))
        w("  %-26s %s" % ("free memory", "%d MB" % vf["free_mb"] if "free_mb" in vf else "NOT READ"))
        b = self.dev.battery()
        if b:
            tc = fnum(b.get("temperature"))
            w("  %-26s %s %%, %s C, status %s (AC %s, USB %s)" % ("battery", b.get("level", "?"), "%.1f" % (tc / 10) if tc is not None else "?",
                                                                 b.get("status", "?"), b.get("AC powered", "?"), b.get("USB powered", "?")))
        else:
            w("  battery                    NOT READ")
            missing.append("battery")
        v["battery"] = b.get("level")
        v["battery_temp"] = (fnum(b.get("temperature")) or 0) / 10 if b.get("temperature") else None
        w("  %-26s %s" % ("resume line (the log)", g.get("resume", "(none this run)")))
        if missing:
            w("\nNOT READ: %s" % "; ".join(missing))
        return "\n".join(out) + "\n", v, missing

    # ---- the loop
    def run(self, stopfile):
        self.dev.start_logcat(self.on_logcat)
        self.poll_log(act=False)                 # what the game said before this started: its build, settings, the level
        if self.rando_order is None:
            # (no run started in this log yet: the saved Randomizer row says whether the order is shuffled)
            ro = re.search(r"^RandoOrder=(\d+)", self.dev.settings_text() or "", re.M)
            if ro:
                self.rando_order = int(ro.group(1))
        self.log("ARMED: %s; session %s" % (self.describe_target(), self.dir))
        if self.cur:
            self.log("the game is in %s now%s" % (level_label(self.cur), " (the main menu)" if self.front else ""))
        while True:
            t = self.dev.now()
            self.poll_log(act=True)
            if self.pending and t - self.pending["t"] >= 2.0:
                self.confirm(t, True)
            c = self.cap
            if c and not self.contact:
                if not c.ending and t - self.lost_at > PAUSE_LIMIT:
                    self.stop("no contact with the headset for more than a minute (asleep: the headset off)", t)
                    c.flag_unsure = True
                if c.ending and t - c.end_t >= 5.0:
                    self.finish()
            elif c:
                if c.ending and t - c.end_t >= 5.0:
                    self.finish()
                elif not c.ending:
                    if not self.pid:
                        self.stop("the game closed (its process ended)", t)
                    elif self.vr_game and t - self.last_fps > PAUSE_LIMIT:
                        self.stop("the app paused or the headset off for more than a minute (no compositor line for %.0f s)" % (t - self.last_fps), t)
                    elif not self.vr_game and t - max(self.last_line, c.t0) > PAUSE_LIMIT + 15:
                        # (no compositor line from the game's process: the game log's quiet instead; it writes a summary a minute)
                        self.stop("the app paused or the headset off for more than a minute (the game log quiet for %.0f s)" % (t - self.last_line), t)
                    else:
                        self.poll_settings(t)
            if os.path.exists(stopfile) or self.dev.finished():
                if self.cap:
                    self.stop("stopped by hand (questperf auto stop)" if os.path.exists(stopfile) else "the feed ended", t)
                    self.finish()
                break
            self.dev.tick()
        self.dev.stop_logcat()
        if os.path.exists(stopfile):
            os.remove(stopfile)
        self.log("STOPPED: %d capture(s) in %s" % (len(self.done), self.dir))
        if len(self.done) >= 2:
            self.log("COMPARE: %s" % compare(self.dir, quiet=True))


# ------------------------------------------------------------------------------------------------ the numbers

def read_csv(p):
    import csv
    if not os.path.exists(p):
        return [], []
    with open(p, encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))
    return rows


def summarize(d, qp):
    meta = json.load(open(os.path.join(d, "capture.json")))
    head, data, name = qp.read_game_capture(d)
    s = {"n": meta["n"], "map": meta["map"], "label": meta["label"] + (" (Performance test)" if meta.get("kind") == "test" else ""),
         "reason": meta["reason"], "deaths": meta["deaths"],
         "changes": meta["changes"], "t0": meta["t0"], "t1": meta["t1"], "settings": meta["settings"], "file": name}
    s["minutes"] = (meta["t1"] - meta["t0"]) / 60.0
    vr = [(t, l) for t, l in json.load(open(os.path.join(d, "vrapi_times.json")))] if os.path.exists(os.path.join(d, "vrapi_times.json")) else []
    vrf = [(t, vr_fields(l)) for t, l in vr if VRAPI.match(l)]
    seen = [f["hz"] for _, f in vrf if "hz" in f]
    hz_seen = statistics.mode(seen) if seen else fnum(meta["settings"].get("Applied: refresh Hz"))
    expect = meta.get("expect_hz") or hz_seen or 72
    hz = int(expect)
    s["hz"], s["hz_seen"] = hz, hz_seen
    s["hz_share"] = 100.0 * sum(1 for x in seen if x == hz) / len(seen) if seen else None
    s["fell_back"] = bool(seen) and s["hz_share"] < 95.0
    budget = 1000.0 / hz
    s["budget"] = budget
    if not head:
        s["error"] = "no capture rows (the game's capture file is missing)"
        return s
    col = {c: i for i, c in enumerate(head)}

    def val(r, c):
        i = col.get(c)
        return fnum(r[i]) if i is not None and i < len(r) else None
    play = [r for r in data if r[col["in_play"]] == "1"] if "in_play" in col else data
    s["frames"], s["play_frames"] = len(data), len(play)
    ts = [val(r, "t_s") for r in data]
    ts = [x for x in ts if x is not None]
    s["t_first"], s["t_last"] = (min(ts), max(ts)) if ts else (0, 0)
    for k in ("frame_ms", "game_ms", "render_ms", "rhi_ms", "gpu_ms", "xr_wait_ms", "draws", "tris"):
        vals = [x for x in (val(r, k) for r in play) if x is not None]
        s[k] = {"avg": avg(vals), "p95": pct(vals, 95), "p99": pct(vals, 99), "max": max(vals) if vals else 0.0}
    fr = [x for x in (val(r, "frame_ms") for r in play) if x is not None]
    s["over_pct"] = 100.0 * sum(1 for x in fr if x > budget * 1.05) / max(1, len(fr))
    # the game thread's own work: its time less the wait for the headset's frame (xr_wait_ms)
    gw = [max(0.0, (val(r, "game_ms") or 0) - (val(r, "xr_wait_ms") or 0)) for r in play]
    s["game_work"] = {"avg": avg(gw), "p95": pct(gw, 95), "p99": pct(gw, 99), "max": max(gw) if gw else 0.0}
    # each frame that missed the budget, by what was over it: a CPU thread, else the GPU (or the compositor)
    miss = {"game thread": 0, "render thread": 0, "RHI thread": 0, "GPU (no CPU thread over)": 0}
    for r in play:
        f = val(r, "frame_ms")
        if f is None or f <= budget * 1.05:
            continue
        g_ = max(0.0, (val(r, "game_ms") or 0) - (val(r, "xr_wait_ms") or 0))
        if g_ > budget:
            miss["game thread"] += 1
        elif (val(r, "render_ms") or 0) > budget:
            miss["render thread"] += 1
        elif (val(r, "rhi_ms") or 0) > budget:
            miss["RHI thread"] += 1
        else:
            miss["GPU (no CPU thread over)"] += 1
    s["misses"] = miss
    first_bucket = head.index("textures_made") + 1 if "textures_made" in head else len(head)
    bk = []
    for c in head[first_bucket:]:
        vals = [x for x in (val(r, c) for r in play) if x is not None]
        if vals and max(vals) > 0:
            bk.append([c, avg(vals), pct(vals, 95)])
    s["buckets"] = sorted(bk, key=lambda b: -b[1])[:8]
    s["over30"] = sum(1 for x in fr if x > 30)
    s["play_s"] = sum(fr) / 1000.0
    s["fps_frames"] = len(fr) / s["play_s"] if s["play_s"] else 0.0
    # per second of the game's clock: live play only, with where the player was (perf_timeline.csv) and the bad guys awake
    secs = {}
    for r in play:
        t, f = val(r, "t_s"), val(r, "frame_ms")
        if t is None or f is None:
            continue
        secs.setdefault(int(t), []).append(f)
    tl = {}
    rows = read_csv(os.path.join(d, "perf_timeline.csv"))
    if rows:
        h = rows[0]
        for r in rows[1:]:
            if len(r) == len(h) and fnum(r[0]) is not None:
                tl[int(fnum(r[0]))] = dict(zip(h, r))
    cn = {}
    rows = read_csv(os.path.join(d, "perf_counts.csv"))
    if rows:
        h = rows[0]
        for r in rows[1:]:
            if len(r) == len(h) and fnum(r[0]) is not None:
                cn[int(fnum(r[0]))] = dict(zip(h, r))

    def near(table, t):
        for dt in (0, -1, 1, -2, 2):
            if t + dt in table:
                return table[t + dt]
        return None
    per = []
    for t in sorted(secs):
        fs = secs[t]
        if len(fs) < 10:
            continue
        row = {"t": t, "frames": len(fs), "avg": avg(fs), "max": max(fs), "over": sum(1 for x in fs if x > budget * 1.05)}
        p = near(tl, t)
        if p:
            row.update(x=fnum(p.get("x")), y=fnum(p.get("y")), z=fnum(p.get("z")), mem=fnum(p.get("mem_mb")), top=p.get("top_bucket"),
                       top_ms=fnum(p.get("top_bucket_ms")))
        q = near(cn, t)
        if q:
            row.update(awake=fnum(q.get("enemies_awake")), sprites=fnum(q.get("sprites_shown")), throttle=fnum(q.get("throttle_level")))
        per.append(row)
    s["per_second"] = per
    s["awake"] = avg([r["awake"] for r in per if r.get("awake") is not None])
    s["awake_max"] = max([r["awake"] for r in per if r.get("awake") is not None] or [0])
    mem = [r["mem"] for r in per if r.get("mem") is not None]
    s["mem_avg"], s["mem_max"] = avg(mem), max(mem or [0])
    s["worst"] = sorted(per, key=lambda r: -r["avg"])[:8]
    # the compositor's seconds that fall in live play: its line's PC time mapped onto the game's clock by the capture's start
    anchor = meta["t0"] + 1.0
    live = set(secs)
    vin = [f for t, f in vrf if int(s["t_first"] + (t - anchor)) in live]
    if not vin:
        vin = [f for _, f in vrf]
    s["vr_secs"] = len(vin)
    s["delivered"] = avg([f["fps"] for f in vin if "fps" in f])
    s["delivered_min"] = min([f["fps"] for f in vin if "fps" in f] or [0])
    s["stale"] = sum(f.get("stale", 0) for f in vin)
    s["vr_gpu_pct"] = 100.0 * avg([f["gpu_pct"] for f in vin if "gpu_pct" in f])
    s["vr_cpu_pct"] = 100.0 * avg([f["cpu_pct"] for f in vin if "cpu_pct" in f])
    s["free_min"] = min([f["free_mb"] for f in vin if "free_mb" in f] or [0])
    s["levels"] = statistics.mode([f["levels"] for f in vin if "levels" in f]) if any("levels" in f for f in vin) else None
    by_min = {}
    for t, f in vrf:
        if "temp" in f:
            by_min.setdefault(int((t - meta["t0"]) // 60), []).append(f["temp"])
    s["temp_by_min"] = [round(max(by_min[k]), 1) for k in sorted(by_min)]
    gm = os.path.join(d, "gpu_metrics.jsonl")
    g = [json.loads(l) for l in open(gm, encoding="utf-8") if l.strip()] if os.path.exists(gm) else []
    gin = [x["m"] for x in g if int(s["t_first"] + (x["t"] - anchor)) in live] or [x["m"] for x in g]
    s["gpu_samples"] = len(gin)
    for k, name in (("gpu_util", "GPU % Utilization"), ("frag", "% Time Shading Fragments"), ("bus", "GPU % Bus Busy"),
                    ("tex_stall", "% Texture Fetch Stall")):
        vals = [x[name] for x in gin if name in x]
        s[k] = {"avg": avg(vals), "p95": pct(vals, 95)} if vals else None
    s["limit"] = limited_by(s)
    return s


def limited_by(s):
    """What holds the run back: what was over the budget in most of the frames that missed it (when 0.5 % or more missed),
    else whichever is nearest its limit (the GPU's busy share against a CPU thread's ms against the budget)."""
    gu = s["gpu_util"]["avg"] if s.get("gpu_util") else s.get("vr_gpu_pct", 0)
    cpu = max((("game thread", s["game_work"]["avg"]), ("render thread", s["render_ms"]["avg"]), ("RHI thread", s["rhi_ms"]["avg"])),
              key=lambda kv: kv[1])
    m = s["misses"]
    total = sum(m.values())
    top = max(m.items(), key=lambda kv: kv[1]) if total else None
    near_gpu = "the GPU (busy %.0f %%)" % gu
    near_cpu = "the %s (%.2f ms of %.2f)" % (cpu[0], cpu[1], s["budget"])
    if top and total >= 0.005 * max(1, s["play_frames"]):
        who = (near_gpu if top[0].startswith("GPU") else "the %s (p95 %.2f ms of %.2f)" % (
            top[0], {"game thread": s["game_work"], "render thread": s["render_ms"], "RHI thread": s["rhi_ms"]}[top[0]]["p95"], s["budget"]))
        who += ", %.0f %% of the misses" % (100.0 * top[1] / total)
    else:
        who = (near_gpu if gu / 100.0 >= cpu[1] / s["budget"] else near_cpu) + ", nothing missing the budget to speak of"
    return {"who": who, "gpu_busy": gu, "cpu": cpu, "miss_top": top[0] if top else None,
            "miss_top_pct": 100.0 * top[1] / total if top else 0.0, "misses": total}


def settings_brief(st):
    def g(k):
        x = st.get("Graphics: " + k)
        return x if x is not None else "default"
    od = st.get("Graphics: Object Detail")
    od = CHOICES["ObjectDetail"][int(od)] if od and od.isdigit() and int(od) < 3 else (od or "default")
    return "Resolution %s (applied %s, eye buffer %s), Refresh %s, Lamp Lights %s, Remastered %s, Object Detail %s, Bad guys x%s" % (
        g("Resolution"), st.get("Applied: resolution") or "?", st.get("Applied: eye buffer") or "?", g("Refresh Rate"), g("Lamp Lights"),
        g("Remastered Graphics"), od, st.get("Gameplay: Bad guys (single player)") or "?")


def fell_back_text(s):
    if not s.get("fell_back"):
        return ""
    return "REFRESH FELL BACK: %d Hz expected, the compositor ran %s Hz (%d Hz in %.0f %% of its seconds); the numbers are against %d Hz. " % (
        s["hz"], s.get("hz_seen"), s["hz"], s.get("hz_share") or 0, s["hz"])


def headline(s):
    if s.get("error"):
        return "capture %d, %s: %s; ended: %s" % (s["n"], s["label"], s["error"], s["reason"])
    gu = s.get("gpu_util")
    lim = s.get("limit") or {}
    return (fell_back_text(s) + "capture %d, %s, %.1f min (%.1f min in live play): %d Hz, budget %.2f ms; frame avg %.2f ms (p95 %.2f, p99 %.2f), "
            "%.1f %% of frames over budget; delivered %.1f fps (min %d), %d stale; game work %.2f, render %.2f, RHI %.2f, GPU %.2f ms; "
            "GPU busy %s; limited by %s; %d death(s); ended: %s; %s" % (
                s["n"], s["label"], s["minutes"], s["play_s"] / 60.0, s["hz"], s["budget"], s["frame_ms"]["avg"], s["frame_ms"]["p95"],
                s["frame_ms"]["p99"], s["over_pct"], s["delivered"], s["delivered_min"], s["stale"], s["game_work"]["avg"], s["render_ms"]["avg"],
                s["rhi_ms"]["avg"], s["gpu_ms"]["avg"], "%.0f %%" % gu["avg"] if gu else "%.0f %% (compositor)" % s["vr_gpu_pct"], lim.get("who", "?"),
                s["deaths"], s["reason"], settings_brief(s["settings"])))


def report_capture(qp, d, s, head):
    json.dump(s, open(os.path.join(d, "summary.json"), "w"), indent=1)
    w = []
    w.append("HEADLINE: " + head)
    w.append("")
    if not s.get("error"):
        w.append("## This capture, live play only (%d of %d frames; budget %.2f ms at %d Hz, a frame over is over %.2f ms)" % (
            s["play_frames"], s["frames"], s["budget"], s["hz"], s["budget"] * 1.05))
        if s.get("fell_back"):
            w.append("  " + fell_back_text(s))
        w.append("  refresh: %d Hz expected, the compositor ran %s Hz (%s %% of its seconds at %d Hz)" % (
            s["hz"], s.get("hz_seen"), "%.0f" % s["hz_share"] if s.get("hz_share") is not None else "?", s["hz"]))
        w.append("  %-10s %9s %9s %9s %9s" % ("", "avg", "p95", "p99", "max"))
        for k in ("frame_ms", "game_ms", "game_work", "render_ms", "rhi_ms", "gpu_ms", "xr_wait_ms", "draws", "tris"):
            x = s[k]
            w.append("  %-10s %9.2f %9.2f %9.2f %9.2f" % (k, x["avg"], x["p95"], x["p99"], x["max"]))
        w.append("  (game_work = game_ms less xr_wait_ms, the game thread's own work; gpu_ms spans waits for the CPU too)")
        lim = s["limit"]
        w.append("  limited by %s; of the %d frames that missed %.2f ms: %s" % (lim["who"], lim["misses"], s["budget"], ", ".join(
            "%s %.0f %%" % (k, 100.0 * v / max(1, lim["misses"])) for k, v in s["misses"].items() if v)) if lim["misses"] else
                 "  limited by %s; no frame missed the budget" % lim["who"])
        if s.get("buckets"):
            w.append("  our costliest CPU buckets (ms a frame, avg / p95): %s" % ", ".join("%s %.2f / %.2f" % (n, a, p) for n, a, p in s["buckets"][:6]))
        w.append("  frames over budget %.1f %%, over 30 ms %d; fps from the frames %.1f; delivered (compositor) %.1f, min %d; stale %d over %d s" % (
            s["over_pct"], s["over30"], s["fps_frames"], s["delivered"], s["delivered_min"], s["stale"], s["vr_secs"]))
        for k, label in (("gpu_util", "GPU utilisation"), ("frag", "fragment share of shading"), ("bus", "GPU bus busy"), ("tex_stall", "texture fetch stall")):
            if s.get(k):
                w.append("  %-28s avg %5.1f %%   p95 %5.1f %%" % (label, s[k]["avg"], s[k]["p95"]))
        w.append("  compositor: GPU %.0f %%, CPU %.0f %%, levels %s, free memory min %d MB; memory used avg %.0f MB, max %.0f MB" % (
            s["vr_gpu_pct"], s["vr_cpu_pct"], s["levels"] or "?", s["free_min"], s["mem_avg"], s["mem_max"]))
        w.append("  temperature by minute (compositor, C): %s" % (", ".join("%g" % x for x in s["temp_by_min"]) or "?"))
        w.append("  bad guys awake: avg %.1f, max %.0f" % (s["awake"], s["awake_max"]))
        w.append("\n## The worst seconds (live play, by their average frame)")
        w.append("  %7s %6s %8s %8s %6s   %-26s %6s   %s" % ("t (s)", "into", "avg ms", "max ms", "over", "where (x, y, z)", "awake", "costliest bucket"))
        for r in s["worst"]:
            w.append("  %7d %6s %8.2f %8.2f %6d   %-26s %6s   %s" % (r["t"], mmss(r["t"] - s["t_first"]), r["avg"], r["max"], r["over"], pos(r),
                                                              "%.0f" % r["awake"] if r.get("awake") is not None else "?",
                                                              "%s %.2f ms" % (r.get("top"), r.get("top_ms") or 0) if r.get("top") else "?"))
    mk = os.path.join(d, "markers.txt")
    if os.path.exists(mk):
        w.append("\n## Markers\n" + open(mk, encoding="utf-8").read().rstrip())
    w.append("\n## Settings at the start (settings.txt)\n" + open(os.path.join(d, "settings.txt"), encoding="utf-8").read().rstrip())
    gs = os.path.join(d, "game_settings.txt")
    if os.path.exists(gs):
        w.append("\n## The game's own settings dump (the Performance test's, %s)\n" % s.get("game_settings_name", "game_settings.txt")
                 + open(gs, encoding="utf-8", errors="replace").read().rstrip())
    w.append("\n\n# The general report over this capture's window\n")
    # (questperf's own report reads the window: the pulled session files cut to the capture's seconds)
    for f in ("perf_timeline.csv", "perf_counts.csv", "hitches.csv"):
        trim(os.path.join(d, f), s.get("t_first", 0) - 1, s.get("t_last", 0) + 1)
    qp.report(d, prefix=w, quiet=True)


def redo(qp, d):
    s = summarize(d, qp)
    head = headline(s)
    report_capture(qp, d, s, head)
    print(head)
    print("report:", os.path.join(d, "report.txt"))


def mmss(sec):
    return "%d:%02d" % (int(sec) // 60, int(sec) % 60)


def pos(r):
    return "(%.0f, %.0f, %.0f)" % (r["x"], r["y"], r["z"]) if r.get("x") is not None else "?"


def trim(p, t0, t1):
    """A session file cut to the capture's seconds (the whole one kept as *_full.csv)."""
    if not os.path.exists(p) or t1 <= t0:
        return
    full = p[:-4] + "_full.csv"
    if not os.path.exists(full):
        shutil.copyfile(p, full)
    lines = open(full, encoding="utf-8", errors="replace").read().splitlines()
    keep = [lines[0]] + [l for l in lines[1:] if fnum(l.split(",", 1)[0]) is not None and t0 <= fnum(l.split(",", 1)[0]) <= t1]
    open(p, "w", encoding="utf-8").write("\n".join(keep) + "\n")


# ------------------------------------------------------------------------------------------------ compare.txt

def compare(sess, quiet=False):
    caps = sorted(glob.glob(os.path.join(sess, "capture_*", "summary.json")))
    S = [json.load(open(p)) for p in caps]
    S = [s for s in S if not s.get("error")]
    out = os.path.join(sess, "compare.txt")
    if len(S) < 2:
        msg = "compare needs two captures with rows (%d here)" % len(S)
        open(out, "w", encoding="utf-8").write(msg + "\n")
        return msg
    w = []
    main = [s for s in S if s["play_s"] >= 60] or S
    a = main[0]
    other = [s for s in main[1:] if settings_brief(s["settings"]) != settings_brief(a["settings"])]
    b = (other or main[1:] or S[1:])[-1]
    d = b["frame_ms"]["avg"] - a["frame_ms"]["avg"]
    head = ("capture %d vs %d (%s): frame avg %.2f -> %.2f ms (%+.2f), p95 %.2f -> %.2f, p99 %.2f -> %.2f; over budget %.1f %% -> %.1f %%; "
            "delivered %.1f -> %.1f fps at %d / %d Hz; GPU busy %s -> %s; bad guys awake %.1f -> %.1f" % (
                a["n"], b["n"], a["label"] if a["map"] == b["map"] else "%s / %s" % (a["label"], b["label"]), a["frame_ms"]["avg"], b["frame_ms"]["avg"], d,
                a["frame_ms"]["p95"], b["frame_ms"]["p95"], a["frame_ms"]["p99"], b["frame_ms"]["p99"], a["over_pct"], b["over_pct"],
                a["delivered"], b["delivered"], a["hz"], b["hz"], gpu_text(a), gpu_text(b), a["awake"], b["awake"]))
    w.append("HEADLINE: " + head)
    w.append("  (the pair: the first and the last capture with a minute of live play or more and other settings than the first)")
    w.append("")
    w.append("## The captures")
    for s in S:
        w.append("  %d. %s, %.1f min (%.1f in live play), ended: %s; %d death(s), %d Graphics change(s)" % (
            s["n"], s["label"], s["minutes"], s["play_s"] / 60, s["reason"], s["deaths"], s["changes"]))
        w.append("     %s" % settings_brief(s["settings"]))
    w.append("\n## Live play, side by side")
    cols = "".join("%14s" % ("capture %d" % s["n"]) for s in S)
    w.append("  %-34s%s" % ("", cols))

    def row(label, f):
        w.append("  %-34s%s" % (label, "".join("%14s" % f(s) for s in S)))
    row("refresh / budget", lambda s: "%d Hz %.2f" % (s["hz"], s["budget"]))
    row("compositor rate (share)", lambda s: "%s (%s %%)" % (s.get("hz_seen"), "%.0f" % s["hz_share"] if s.get("hz_share") is not None else "?"))
    row("limited by", lambda s: (s.get("limit") or {}).get("who", "?").split(" (")[0].replace("the ", ""))
    row("game thread work avg (p95)", lambda s: "%.2f (%.2f)" % (s["game_work"]["avg"], s["game_work"]["p95"]))
    row("delivered fps (min)", lambda s: "%.1f (%d)" % (s["delivered"], s["delivered_min"]))
    row("fps from the frames", lambda s: "%.1f" % s["fps_frames"])
    for k in ("frame_ms", "game_ms", "render_ms", "rhi_ms", "gpu_ms"):
        for q in ("avg", "p95", "p99"):
            row("%s %s" % (k, q), lambda s, k=k, q=q: "%.2f" % s[k][q])
    row("% frames over budget", lambda s: "%.1f" % s["over_pct"])
    row("frames over 30 ms", lambda s: "%d" % s["over30"])
    row("stale frames (per min)", lambda s: "%d (%.1f)" % (s["stale"], s["stale"] / max(0.1, s["vr_secs"] / 60)))
    row("GPU utilisation % (p95)", lambda s: "%.0f (%.0f)" % (s["gpu_util"]["avg"], s["gpu_util"]["p95"]) if s.get("gpu_util") else "-")
    row("compositor GPU % / CPU %", lambda s: "%.0f / %.0f" % (s["vr_gpu_pct"], s["vr_cpu_pct"]))
    row("fragment share %", lambda s: "%.1f" % s["frag"]["avg"] if s.get("frag") else "-")
    row("GPU bus busy %", lambda s: "%.0f" % s["bus"]["avg"] if s.get("bus") else "-")
    row("draws avg (p95)", lambda s: "%.0f (%.0f)" % (s["draws"]["avg"], s["draws"]["p95"]))
    row("tris avg (p95), k", lambda s: "%.0f (%.0f)" % (s["tris"]["avg"] / 1000, s["tris"]["p95"] / 1000))
    row("memory used MB avg / max", lambda s: "%.0f / %.0f" % (s["mem_avg"], s["mem_max"]))
    row("free memory min MB", lambda s: "%d" % s["free_min"])
    row("CPU / GPU levels", lambda s: (s["levels"] or "?").split(" (")[0])
    row("bad guys awake avg (max)", lambda s: "%.1f (%.0f)" % (s["awake"], s["awake_max"]))
    w.append("\n## Temperature by minute (compositor, C)")
    for s in S:
        w.append("  capture %d: %s" % (s["n"], ", ".join("%g" % x for x in s["temp_by_min"]) or "?"))
    # where the runs went: 10 m squares of the map
    def cells(s):
        return {(int(r["x"] // 1000), int(r["y"] // 1000)) for r in s["per_second"] if r.get("x") is not None}
    w.append("\n## The worst moments, matched by area (10 m around the spot, in the other captures' live play)")
    for s in S:
        others = [o for o in S if o is not s and o["map"] == s["map"]]
        w.append("  capture %d's worst seconds:" % s["n"])
        for r in s["worst"][:6]:
            parts = []
            for o in others:
                near = [q for q in o["per_second"] if q.get("x") is not None and r.get("x") is not None
                        and abs(q["x"] - r["x"]) <= 1000 and abs(q["y"] - r["y"]) <= 1000 and abs(q["z"] - r["z"]) <= 600]
                if near:
                    parts.append("capture %d there: %.2f ms avg over %d s (worst %.2f), %.1f awake" % (
                        o["n"], avg([q["avg"] for q in near]), len(near), max(q["avg"] for q in near), avg([q["awake"] for q in near if q.get("awake") is not None])))
                else:
                    parts.append("capture %d never went there" % o["n"])
            w.append("    %s in %-26s %.2f ms (max %.1f), %s awake | %s" % (mmss(r["t"] - s["t_first"]), pos(r), r["avg"], r["max"],
                                                                       "%.0f" % r["awake"] if r.get("awake") is not None else "?", "; ".join(parts) or "-"))
    # the play path against the settings
    w.append("\n## What limits each run (against %d Hz, %.2f ms)" % (b["hz"], b["budget"]))
    for s in S:
        lim = s["limit"]
        w.append("  capture %d: %s%s; misses %.1f %% of frames: %s" % (s["n"], fell_back_text(s), lim["who"], s["over_pct"], ", ".join(
            "%s %.0f %%" % (k, 100.0 * v / max(1, lim["misses"])) for k, v in s["misses"].items() if v) or "none"))
    lo = min((a, b), key=lambda s: settings_score(s["settings"]))
    lim = lo["limit"]
    w.append("\n## What holds %d Hz back at the lowest settings (capture %d: %s)" % (lo["hz"], lo["n"], settings_brief(lo["settings"])))
    if not lim["misses"]:
        w.append("  Nothing: no frame missed %.2f ms in live play." % lo["budget"])
    else:
        w.append("  %.1f %% of the frames miss %.2f ms (p95 %.2f, p99 %.2f ms); delivered %.1f of %d fps, %d stale." % (
            lo["over_pct"], lo["budget"], lo["frame_ms"]["p95"], lo["frame_ms"]["p99"], lo["delivered"], lo["hz"], lo["stale"]))
        w.append("  The misses: %s." % ", ".join("%s %.0f %%" % (k, 100.0 * v / lim["misses"]) for k, v in lo["misses"].items() if v))
        w.append("  CPU threads against the budget: game work %.2f ms avg (p95 %.2f), render %.2f (p95 %.2f), RHI %.2f (p95 %.2f); GPU busy %.0f %%." % (
            lo["game_work"]["avg"], lo["game_work"]["p95"], lo["render_ms"]["avg"], lo["render_ms"]["p95"], lo["rhi_ms"]["avg"], lo["rhi_ms"]["p95"],
            lim["gpu_busy"]))
        if lo.get("buckets"):
            w.append("  The game thread's costliest buckets: %s." % ", ".join("%s %.2f ms (p95 %.2f)" % (n, a_, p_) for n, a_, p_ in lo["buckets"][:6]))
        top = lim["miss_top"]
        if top and top.startswith("GPU"):
            w.append("  So the GPU holds it back even at these settings: the lowest resolution and lamps still leave its frame over %.2f ms." % lo["budget"])
        elif top:
            w.append("  So the %s holds it back: lower graphics settings cannot help those frames; its work (the buckets above) has to shrink." % top)
    w.append("\n## What is down to the play path, not the settings")
    ca, cb = cells(a), cells(b)
    both = ca & cb
    shared_a = [r for r in a["per_second"] if r.get("x") is not None and (int(r["x"] // 1000), int(r["y"] // 1000)) in both]
    shared_b = [r for r in b["per_second"] if r.get("x") is not None and (int(r["x"] // 1000), int(r["y"] // 1000)) in both]
    w.append("  Route: capture %d covered %d squares of 10 m, capture %d %d; %d in both (%.0f %% of capture %d's)." % (
        a["n"], len(ca), b["n"], len(cb), len(both), 100.0 * len(both) / max(1, len(ca)), a["n"]))
    if shared_a and shared_b:
        da = avg([r["avg"] for r in shared_a])
        db = avg([r["avg"] for r in shared_b])
        w.append("  On the ground both covered: frame avg %.2f -> %.2f ms (%+.2f); over the whole runs %.2f -> %.2f (%+.2f)." % (
            da, db, db - da, a["frame_ms"]["avg"], b["frame_ms"]["avg"], d))
        w.append("  The like-for-like number is the first: the same places, so most of what is left there is the settings.")
        aw_a = avg([r["awake"] for r in shared_a if r.get("awake") is not None])
        aw_b = avg([r["awake"] for r in shared_b if r.get("awake") is not None])
        w.append("  Bad guys awake on that ground: %.1f -> %.1f; over the whole runs %.1f -> %.1f." % (aw_a, aw_b, a["awake"], b["awake"]))
        if abs(aw_b - aw_a) >= 1.0:
            w.append("  The bad guys on screen differ between the runs (who was alive where, how fast the level was cleared): part of the game-thread")
            w.append("  and draw differences is that, not the settings. Compare game ms (the bad guys' cost) with render and GPU ms (the settings' cost).")
    w.append("  Time in live play: %.1f -> %.1f min; deaths %d -> %d (a death's rebuild and the death view are left out, but the level's state after a" % (
        a["play_s"] / 60, b["play_s"] / 60, a["deaths"], b["deaths"]))
    w.append("  respawn differs: bad guys back, pickups back).")
    gpu_a, gpu_b = a.get("gpu_util"), b.get("gpu_util")
    if gpu_a and gpu_b:
        w.append("  GPU utilisation %.0f %% -> %.0f %%: the settings' load on the GPU (resolution, lamps, refresh); the CPU threads' share is the play." % (
            gpu_a["avg"], gpu_b["avg"]))
    open(out, "w", encoding="utf-8").write("\n".join(w) + "\n")
    if not quiet:
        print("\n".join(w))
        print("\ncompare:", out)
    return head


def settings_score(st):
    """Higher = heavier graphics (to name the lowest-settings run)."""
    def n(k, d=0.0):
        x = st.get("Graphics: " + k)
        if x in ("true", "True"):
            return 1.0
        if x in ("false", "False"):
            return 0.0
        return fnum(x) if fnum(x) is not None else d
    return n("Resolution", 100) + 5 * n("Lamp Lights", 4) + 10 * n("Object Detail", 1) + 10 * n("Remastered Graphics", 1) + 3 * n("Stochastic Textures", 1)


def gpu_text(s):
    return "%.0f %%" % s["gpu_util"]["avg"] if s.get("gpu_util") else "%.0f %% (comp.)" % s["vr_gpu_pct"]


# ------------------------------------------------------------------------------------------------ entry

def main(qp, argv):
    stopfile = os.path.join(qp.OUT, "auto.stop")
    if argv and argv[0] == "compare":
        sess = argv[1] if len(argv) > 1 else (sorted(glob.glob(os.path.join(qp.OUT, "auto_*"))) or [None])[-1]
        if not sess:
            sys.exit("no auto sessions in " + qp.OUT)
        compare(sess)
        return
    args = argv[1:]
    if args and args[0] == "stop":
        os.makedirs(qp.OUT, exist_ok=True)
        open(stopfile, "w").write("stop\n")
        print("asked the armed session to stop (it finishes a capture under way first)")
        return
    level_n, map_name, fake, out, hz = 2, None, None, None, None
    i = 0
    while i < len(args):
        if args[i] == "--level":
            level_n = int(args[i + 1])
            i += 2
        elif args[i] == "--map":
            map_name = args[i + 1]
            i += 2
        elif args[i] == "--fake":
            fake = args[i + 1]
            i += 2
        elif args[i] == "--out":
            out = args[i + 1]
            i += 2
        elif args[i] == "--hz":
            hz = int(args[i + 1])
            i += 2
        else:
            sys.exit(__doc__)
    if os.path.exists(stopfile):
        os.remove(stopfile)
    if fake:
        import questperf_fake
        dev = questperf_fake.FakeQuest(fake)
    else:
        if not qp.require_quest():
            return
        dev = Quest(qp)
    a = Auto(qp, dev, level_n, map_name, out, hz)
    try:
        a.run(stopfile)
    except KeyboardInterrupt:
        if a.cap:
            a.stop("stopped by hand (Ctrl+C)", dev.now())
            a.finish()
        a.log("STOPPED (Ctrl+C): %d capture(s) in %s" % (len(a.done), a.dir))
