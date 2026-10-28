"""Outlaws VR performance captures on the Quest (the LE/VE perf-watch idea, for the headset build).

  python questperf.py record          start: logcat, the GPU's hardware counters (ovrgpuprofiler), the game's own
                                      row-a-frame capture (Saved/Perf/capture.on, Shipping and Development) and, in a
                                      Development build, UE's per-frame CSV (csvprofile), all into a new session folder
  python questperf.py stop            stop them, pull Saved/Perf, the captures and the game's log, write report.txt
  python questperf.py capture on|off  just the game's own capture (the flag file), no logcat or GPU counters
  python questperf.py report [DIR]    (re)build report.txt for a session folder (default: the newest)
  python questperf.py pull            just pull Saved/Perf (performance.txt, perf_live.txt, hang.txt, timeline, buckets, counts, hitches)
  python questperf.py trace [SECS]    an Unreal Insights trace (default 30 s), exported to per-function timings
  python questperf.py mark            a manual "lag right now" mark (Development; in the headset, any build: both sticks held 1.5 s)
  python questperf.py cmd "stat fps"  any console command (Development builds: Shipping has no console)
  python questperf.py auto [--level N | --map NAME]   armed: every start of that story level (default 2, The Town) is a
                                      capture of its own, stopped at its end, with settings.txt and report.txt each
  python questperf.py auto stop       stop an armed auto session;  compare [DIR]: compare.txt over its captures

Sessions go to tools/olue/out/questperf/<date_time>/ (auto: auto_<date_time>/).  The game side is OutlawsPerf (OutlawsPerf.h).
Only a Quest is used (the manufacturer Oculus or Meta): another Android device on adb is never picked.
adb: OL_ADB, else ANDROID_HOME's platform-tools, else adb on the PATH.
Untethered: `quest_wifi.py setup` once with the cable, then everything here works over Wi-Fi.
"""
import csv
import datetime
import glob
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "out", "questperf")
STATE = os.path.join(OUT, "current.json")
def find_adb():
    """adb: the OL_ADB environment variable, else ANDROID_HOME's platform-tools, else adb on the PATH."""
    if os.environ.get("OL_ADB"):
        return os.environ["OL_ADB"]
    home = os.environ.get("ANDROID_HOME")
    if home:
        p = os.path.join(home, "platform-tools", "adb.exe" if os.name == "nt" else "adb")
        if os.path.exists(p):
            return p
    return "adb"


ADB = find_adb()
PKG = "com.outlawsvr.OutlawsUE"
SAVED = "/sdcard/Android/data/%s/files/UnrealGame/OutlawsUE/OutlawsUE/Saved" % PKG
# ovrgpuprofiler realtime metrics: utilisation, clock, memory traffic and stalls, where the shader time goes
GPU_METRICS = "2,3,4,8,11,12,13,17,25,32,39,41,64"
LOGCAT_FILTER = ["VrApi:I", "UE:D", "kgsl-3d0:*", "adreno-gen7-gmu:*", "ActivityManager:I", "lowmemorykiller:I",
                 "DEBUG:*", "libc:*", "*:F"]            # *:F keeps the GPU driver's fatal fault lines


def adb(*args, check=False, capture=True):
    r = subprocess.run([ADB] + list(args), capture_output=capture, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode:
        sys.exit("adb %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return (r.stdout or "").strip()


def console(cmd):
    """UE's GameActivity runs android.intent.action.RUN broadcasts as console commands (only while it runs:
    the broadcast itself always "completes", so the game's process is what says it was heard)."""
    if not game_running():
        return False
    adb("shell", "am broadcast -a android.intent.action.RUN -e cmd '%s'" % cmd.replace("'", ""))
    return True


def game_running():
    return bool(adb("shell", "pidof", PKG))


# PERF-SHIPSUMMARY: the game's own detailed capture (OutlawsPerf), in Shipping too where csvprofile and the console are
# compiled out: a row a frame into Saved/Perf/capture_<date_time>.csv while Saved/Perf/capture.on exists.  The game looks
# for the file once a second, so it can be made before the game starts (it starts capturing when it finds it).
CAPTURE_ON = SAVED + "/Perf/capture.on"


def device_captures():
    listing = adb("shell", "ls %s/Perf/ 2>/dev/null" % SAVED)
    return sorted(set(re.findall(r"(capture_\d{8}_\d{6}\.csv)", listing)))


def capture_flag(on):
    if on:
        adb("shell", "mkdir -p %s/Perf && touch %s" % (SAVED, CAPTURE_ON))
    else:
        adb("shell", "rm -f %s" % CAPTURE_ON)
    return adb("shell", "ls %s 2>/dev/null" % CAPTURE_ON) != ""


def spawn(args, logfile):
    """A detached helper that outlives this command (stopped again by `stop`)."""
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS if os.name == "nt" else 0
    return subprocess.Popen(args, stdout=open(logfile, "w", encoding="utf-8"), stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, creationflags=flags).pid


def gpu_sidecar(outfile):
    """Runs detached: stamps each one-second block of ovrgpuprofiler output with the wall clock.  The profiler only
    reads the game's counters when it starts with the game running, so it waits for the game and restarts whenever
    the game's process changes (a relaunch, a reinstall)."""
    with open(outfile, "w", encoding="utf-8") as f:
        while True:
            pid = adb("shell", "pidof", PKG)
            if not pid:
                time.sleep(2)
                continue
            p = subprocess.Popen([ADB, "shell", "ovrgpuprofiler -r'%s'" % GPU_METRICS], stdout=subprocess.PIPE, text=True,
                                 encoding="utf-8", errors="replace")
            watch_pid(p, pid)
            read_gpu_blocks(p, f)


def watch_pid(p, pid):
    """Stops the profiler when the game's process goes away, so the sidecar starts a fresh one."""
    import threading
    def run():
        while p.poll() is None:
            time.sleep(3)
            if adb("shell", "pidof", PKG) != pid:
                p.kill()
                return
    threading.Thread(target=run, daemon=True).start()


def read_gpu_blocks(p, f):
    """ovrgpuprofiler's blank-line-separated blocks, one a second, as JSON lines."""
    block = []
    for line in p.stdout:
        line = line.strip()
        if not line:
            if block:
                f.write(json.dumps({"t": time.time(), "m": dict(block)}) + "\n")
                f.flush()
                block = []
            continue
        if ":" in line:
            k, v = line.rsplit(":", 1)
            try:
                block.append((k.strip(), float(v)))
            except ValueError:
                pass


def kill(pid):
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


# ------------------------------------------------------------------------------------------------ commands

def cmd_record():
    if adb("get-state") != "device":
        sys.exit("no headset on adb (plug it in and allow USB debugging, or `quest_wifi.py setup` / `connect` for Wi-Fi)")
    if os.path.exists(STATE):
        print("a capture is already running (%s); stopping it first" % json.load(open(STATE))["dir"])
        cmd_stop()
    d = os.path.join(OUT, datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(d)
    adb("logcat", "-G", "16M")
    adb("logcat", "-c")
    st = {"dir": d, "start": time.time(), "pids": []}
    st["pids"].append(spawn([ADB, "logcat", "-v", "time"] + LOGCAT_FILTER, os.path.join(d, "logcat.txt")))
    st["pids"].append(spawn([sys.executable, os.path.abspath(__file__), "_gpu", os.path.join(d, "gpu_metrics.jsonl")],
                            os.path.join(d, "gpu_sidecar.log")))
    st["captures_before"] = device_captures()
    st["flag"] = capture_flag(True)
    console("r.GPUCsvStatsEnabled 1")              # the per-pass GPU/ columns (needs timestamp queries on)
    st["csv"] = console("csvprofile start")        # (a Development build only: Shipping has no CSV profiler and no console)
    json.dump(st, open(STATE, "w"))
    print("recording into", d)
    print("  the game's own capture:", ("on (Saved/Perf/capture.on made; the game picks it up within a second, or when it starts)"
                                        if st["flag"] else "NOT on: could not make %s" % CAPTURE_ON))
    print("  UE CSV capture (Development builds):", "asked for" if st["csv"] else "not asked (the game is not running)")
    print("  play; in the headset hold both sticks in for 1.5 s to mark a lag moment (or `questperf.py mark`)")
    print("  then: python questperf.py stop")


def cmd_stop():
    if not os.path.exists(STATE):
        sys.exit("nothing is recording")
    st = json.load(open(STATE))
    capture_flag(False)
    if game_running():
        console("csvprofile stop")
        print("stopping the captures (a few seconds to write) ...")
        time.sleep(4)
    for pid in st["pids"]:
        kill(pid)
    os.remove(STATE)
    pull(st["dir"], since=st["start"], captures_before=st.get("captures_before"))
    report(st["dir"])


def cmd_capture(on):
    ok = capture_flag(on)
    if on:
        print("capture.on made: the game captures a row a frame from within a second (or from its start)" if ok else
              "could not make " + CAPTURE_ON)
    else:
        print("capture.on removed: the game stops within a second; `questperf.py pull` brings the capture" if not ok else
              "could not remove " + CAPTURE_ON)


def pull(d, since=0.0, captures_before=None):
    os.makedirs(d, exist_ok=True)
    for name in ("performance.txt", "perf_live.txt", "perf_timeline.csv", "perf_timeline_prev.csv", "perf_buckets.csv",
                 "perf_counts.csv", "hitches.csv", "hang.txt"):
        adb("pull", "%s/Perf/%s" % (SAVED, name), os.path.join(d, name))
    # the game's own captures: the ones made during this session (not on the headset before `record`), else the newest
    caps = device_captures()
    new = [c for c in caps if captures_before is not None and c not in captures_before]
    for c in (new or caps[-1:]):
        adb("pull", "%s/Perf/%s" % (SAVED, c), os.path.join(d, c))
    # the game's log (Shipping: OutlawsShipLog's file, the game's own lines; the OLPERF summary lines are in it)
    adb("pull", "%s/Logs/OutlawsUE.log" % SAVED, os.path.join(d, "game.log"))
    listing = adb("shell", "ls -l %s/Profiling/CSV 2>/dev/null" % SAVED)
    csvs = re.findall(r"(\S+\.csv(?:\.gz)?)\s*$", listing, re.M)
    # only a capture from this session: the names carry its start time, Profile(YYYYMMDD_HHMMSS).csv
    since_name = datetime.datetime.fromtimestamp(since).strftime("%Y%m%d_%H%M%S") if since else ""
    csvs = [c for c in csvs if (re.search(r"(\d{8}_\d{6})", c) or [None, ""])[1] >= since_name]
    if csvs:
        newest = sorted(csvs)[-1]
        adb("pull", "%s/Profiling/CSV/%s" % (SAVED, newest), os.path.join(d, "ue_" + newest))
    print("pulled into", d, ":", ", ".join(sorted(os.listdir(d))))


# ------------------------------------------------------------------------------------------------ the report

def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p / 100.0 * len(s)))]


def read_timeline(d):
    p = os.path.join(d, "perf_timeline.csv")
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8", errors="replace") as f:
        return [r for r in csv.DictReader(f) if fnum(r.get("frame_ms"))]


def read_ue_csv(d):
    """UE's csvprofile output: one row per frame; metadata rows at the end are skipped."""
    files = sorted(glob.glob(os.path.join(d, "ue_*.csv")))
    if not files:
        return None, []
    with open(files[-1], encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))
    if not rows:
        return None, []
    # the first column is EVENTS (text); with [HasHeaderRowAtEnd] the complete header is repeated at the end
    # (columns can appear mid-capture, so early rows are shorter)
    heads = [r for r in rows if r and r[0] == "EVENTS"]
    head = heads[-1] if heads else rows[0]
    # UE writes stat names unquoted, so a name with commas ("Outlaws/Eng: world end (timers, GC, streaming)") splits
    # into several header fields and shifts every later column name against its data: rejoin unbalanced parentheses
    fixed = []
    for c in head:
        if fixed and fixed[-1].count("(") > fixed[-1].count(")"):
            fixed[-1] += "," + c
        else:
            fixed.append(c)
    head = fixed
    data =[r for r in rows[1:] if r and r[0] != "EVENTS" and not r[0].startswith("[") and len(r) > 1]
    return head, data


def read_game_capture(d):
    """OutlawsPerf's capture_<date_time>.csv: a header row (t_s,...), a row a frame, and the header again at the end
    (a bucket first run during the capture adds a column to the rows after it): the last header names the columns."""
    files = sorted(glob.glob(os.path.join(d, "capture_*.csv")))
    if not files:
        return None, [], ""
    with open(files[-1], encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))
    heads = [r for r in rows if r and r[0] == "t_s"]
    if not heads:
        return None, [], ""
    data = [r for r in rows if r and r[0] != "t_s" and fnum(r[0]) is not None]
    return heads[-1], data, os.path.basename(files[-1])


def column_stats(head, data, name):
    if name not in head:
        return None
    i = head.index(name)
    vals = [v for v in (fnum(r[i]) for r in data if i < len(r)) if v is not None]
    if not vals:
        return None
    return statistics.fmean(vals), pct(vals, 95), max(vals)


def report(d, prefix=None, quiet=False):
    lines = list(prefix or [])
    w = lines.append
    w("Outlaws VR performance report: %s" % os.path.basename(d))
    w("=" * 100)
    budget = 1000.0 / 72

    tl = read_timeline(d)
    # live play only: seconds holding a load or a pause (a frame over 250 ms) would swamp the averages
    play = [r for r in tl if r.get("in_play") == "1" and (fnum(r.get("frame_max_ms")) or 0) < 250]
    if tl:
        w("\n## Session (Saved/Perf/perf_timeline.csv, one row a second; live play only below)")
        w("  %d s recorded, %d s in live play" % (len(tl), len(play)))
        levels = {}
        for r in play:
            levels.setdefault(r["level"], []).append(r)
        w("\n  %-10s %6s %7s %8s %8s %8s %8s %8s %9s %8s   %s" % ("level", "secs", "fps", "frame", "game", "render", "RHI", "GPU", "draws", "tris", "limited by"))
        for lv, rs in sorted(levels.items(), key=lambda kv: -len(kv[1])):
            avg = lambda k: statistics.fmean(fnum(r[k]) or 0.0 for r in rs)
            parts = {"game": avg("game_ms"), "render": avg("render_ms"), "RHI": avg("rhi_ms"), "GPU": avg("gpu_ms")}
            # the GPU's timestamps span the frame's first to last command, waits for the CPU included, so its number
            # follows the frame time: name the busiest CPU thread; the GPU's real load is in the counters below
            bound = max(("game", "render", "RHI"), key=parts.get)
            w("  %-10s %6d %7.1f %8.2f %8.2f %8.2f %8.2f %8.2f %9.0f %8.0f   %s (%.1f ms)" % (
                lv, len(rs), avg("fps"), avg("frame_ms"), parts["game"], parts["render"], parts["RHI"], parts["GPU"],
                avg("draws"), avg("tris"), bound, parts[bound]))
        w("  (budget %.1f ms at 72 Hz; \"limited by\" = the busiest CPU thread. GPU ms includes waiting for the CPU:" % budget)
        w("   its real work is the GPU passes' Total and the utilisation below)")
        worst = sorted([r for r in tl if r.get("in_play") == "1"], key=lambda r: -(fnum(r["frame_max_ms"]) or 0))[:12]
        if worst:
            w("\n## Worst seconds (by their longest frame)")
            w("  %7s %-10s %8s %8s %8s %8s   %-28s %s" % ("t (s)", "level", "max ms", "avg ms", "GPU", "game", "where (x, y, z)", "costliest bucket"))
            for r in worst:
                w("  %7s %-10s %8.1f %8.1f %8.1f %8.1f   %-28s %s %.2f ms" % (
                    r["t_s"], r["level"], fnum(r["frame_max_ms"]), fnum(r["frame_ms"]), fnum(r["gpu_ms"]), fnum(r["game_ms"]),
                    "(%s, %s, %s)" % (r["x"], r["y"], r["z"]), r["top_bucket"], fnum(r["top_bucket_ms"]) or 0))
        spikes = [r for r in play if (fnum(r["textures_made"]) or 0) > 0]
        if spikes:
            w("\n  seconds that made runtime textures (BC7 decode / uploads can hitch): %d, most %s in one second" % (
                len(spikes), max(int(fnum(r["textures_made"])) for r in spikes)))
        gpu_zero = all((fnum(r["gpu_ms"]) or 0) == 0 for r in play) if play else True
        if gpu_zero:
            w("  NOTE: GPU time reads 0: timestamp queries are off (Config/Android/AndroidEngine.ini r.Vulkan.SupportsTimestampQueries=1)")

    head, data = read_ue_csv(d)
    if head:
        w("\n## UE per-frame capture (csvprofile, %d frames)" % len(data))
        for name in ("FrameTime", "GameThreadTime", "RenderThreadTime", "RHIThreadTime", "GPUTime"):
            s = column_stats(head, data, name)
            if s:
                w("  %-18s avg %7.2f   p95 %7.2f   max %7.2f ms" % (name, *s))
        ft = column_stats(head, data, "FrameTime")
        if ft and "FrameTime" in head:
            i = head.index("FrameTime")
            vals = [fnum(r[i]) for r in data if fnum(r[i]) is not None]
            over = sum(1 for v in vals if v > budget * 1.05)
            w("  frames over the %.1f ms budget: %d of %d (%.1f%%)" % (budget, over, len(vals), 100.0 * over / max(1, len(vals))))
        for prefix, title in (("GPU/", "GPU passes (ms/frame)"), ("Outlaws/", "our CPU buckets (ms/frame, game thread)")):
            cols = [c for c in head if c.startswith(prefix)]
            stats = [(c[len(prefix):], column_stats(head, data, c)) for c in cols]
            stats = [(n, s) for n, s in stats if s and s[2] > 0]
            if stats:
                w("\n  %s, costliest first" % title)
                w("    %9s %9s %9s   %s" % ("avg", "p95", "max", "name"))
                for n, s in sorted(stats, key=lambda x: -x[1][0])[:25]:
                    w("    %9.3f %9.3f %9.3f   %s" % (s[0], s[1], s[2], n))
        extra = [c for c in head if c.startswith("Exclusive/GameThread/")]
        if extra:
            stats = [(c.split("/", 2)[2], column_stats(head, data, c)) for c in extra]
            stats = sorted([(n, s) for n, s in stats if s], key=lambda x: -x[1][0])[:15]
            w("\n  engine game-thread work (exclusive ms/frame), costliest first")
            for n, s in stats:
                w("    %9.3f %9.3f %9.3f   %s" % (s[0], s[1], s[2], n))
    ghead, gdata, gname = read_game_capture(d)
    if ghead:
        play_rows = [r for r in gdata if r[ghead.index("in_play")] == "1"] if "in_play" in ghead else gdata
        w("\n## The game's own capture (%s: %d frames, %d in live play; live play below)" % (gname, len(gdata), len(play_rows)))
        for name in ("frame_ms", "game_ms", "render_ms", "rhi_ms", "gpu_ms", "xr_wait_ms", "gc_ms", "draws", "tris"):
            s = column_stats(ghead, play_rows, name)
            if s:
                w("  %-12s avg %8.2f   p95 %8.2f   max %8.2f" % (name, *s))
        if "frame_ms" in ghead:
            i = ghead.index("frame_ms")
            vals = [v for v in (fnum(r[i]) for r in play_rows if i < len(r)) if v is not None]
            over = sum(1 for v in vals if v > budget * 1.05)
            w("  frames over the %.1f ms budget: %d of %d (%.1f%%); over 30 ms: %d" % (
                budget, over, len(vals), 100.0 * over / max(1, len(vals)), sum(1 for v in vals if v > 30)))
            w("  (gpu_ms spans the frame's first to last GPU command, waits for the CPU included)")
        first_bucket = ghead.index("textures_made") + 1 if "textures_made" in ghead else len(ghead)
        stats = [(c, column_stats(ghead, play_rows, c)) for c in ghead[first_bucket:]]
        stats = [(n, s) for n, s in stats if s and s[2] > 0]
        if stats:
            w("\n  our CPU buckets (ms/frame, game thread), costliest first")
            w("    %9s %9s %9s   %s" % ("avg", "p95", "max", "name"))
            for n, s in sorted(stats, key=lambda x: -x[1][0])[:25]:
                w("    %9.3f %9.3f %9.3f   %s" % (s[0], s[1], s[2], n))

    if not head and not ghead:
        live = os.path.join(d, "perf_live.txt")
        if os.path.exists(live):
            w("\n## No per-frame capture in this session; the last perf_live.txt instead")
            w(open(live, encoding="utf-8", errors="replace").read())

    gl = os.path.join(d, "game.log")
    if os.path.exists(gl):
        lines_s = [l.strip() for l in open(gl, encoding="utf-8", errors="replace") if "OLPERF summary" in l]
        if lines_s:
            w("\n## The game's summary lines (game.log, every 60 s and at each level's end; the last 30)")
            for l in lines_s[-30:]:
                w("  " + l[l.index("OLPERF summary"):])

    gm = os.path.join(d, "gpu_metrics.jsonl")
    if os.path.exists(gm):
        samples = [json.loads(l) for l in open(gm, encoding="utf-8") if l.strip()]
        if samples:
            w("\n## GPU hardware counters (ovrgpuprofiler, %d one-second samples)" % len(samples))
            keys = sorted({k for s in samples for k in s["m"]})
            for k in keys:
                vals = [s["m"][k] for s in samples if k in s["m"]]
                scale, unit = (1e6, " M") if max(vals) > 1e6 else (1, "")
                w("  %-42s avg %10.2f%s   p95 %10.2f%s   max %10.2f%s" % (k, statistics.fmean(vals) / scale, unit,
                                                                           pct(vals, 95) / scale, unit, max(vals) / scale, unit))
            w("  (utilisation near 100%% = GPU bound; high bus busy / stalled on memory = bandwidth: textures, overdraw, MSAA)")

    lc = os.path.join(d, "logcat.txt")
    if os.path.exists(lc):
        text = open(lc, encoding="utf-8", errors="replace").read()
        vr = re.findall(r"VrApi.*?FPS=(\d+)/(\d+).*?Stale=(\d+)", text)
        if vr:
            fps = [int(a) for a, _, _ in vr]
            stale = sum(int(c) for _, _, c in vr)
            temps = [float(t) for t in re.findall(r"Temp=([\d.]+)C", text)]
            w("\n## Headset compositor (VrApi, once a second)")
            w("  delivered fps avg %.1f, min %d; stale frames (the compositor reused an old frame) %d over %d s%s" % (
                statistics.fmean(fps), min(fps), stale, len(vr), ", temperature max %.1f C" % max(temps) if temps else ""))
        faults = re.findall(r"^(\S+ \S+) E/kgsl-3d0.*ctx_type VK.*$", text, re.M)
        timeouts = len(re.findall(r"Timed out while waiting for GPU", text))
        if faults or timeouts:
            w("\n## GPU FAULTS: %d (first at %s); the game waited on a dead GPU %d times" % (len(faults), faults[0] if faults else "-", timeouts))

    for name, title in (("performance.txt", "Lag moments and manual marks (this session's block of performance.txt)"), ("hang.txt", "Hang reports")):
        p = os.path.join(d, name)
        if os.path.exists(p):
            body = open(p, encoding="utf-8", errors="replace").read()
            if name == "performance.txt":
                blocks = body.split("=== session ")
                body = ("=== session " + blocks[1]) if len(blocks) > 1 else body
            w("\n## %s\n%s" % (title, body.strip()))

    out = os.path.join(d, "report.txt")
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    if quiet:
        return
    print("\n".join(lines[:60]))
    if len(lines) > 60:
        print("... (%d more lines)" % (len(lines) - 60))
    print("\nreport:", out)


# Unreal Insights: UNREAL_INSIGHTS, else UE_ROOT's (the engine's folder)
INSIGHTS = os.environ.get("UNREAL_INSIGHTS") or os.path.join(os.environ.get("UE_ROOT", "C:/Program Files/Epic Games/UE_5.1"), "Engine", "Binaries",
                                                             "Win64", "UnrealInsights.exe")


def cmd_trace(seconds):
    """A short Unreal Insights trace on the headset (every engine and game scope, named stat events on), pulled and
    exported headless: what the CSV's lumps (WorldTickMisc, TickActors) are made of."""
    if not console("stat namedevents"):
        sys.exit("the game is not running")
    name = "olperf_%s.utrace" % datetime.datetime.now().strftime("%H%M%S")
    console("Trace.File %s cpu,frame,bookmark,log" % name)
    print("tracing for %d s: play normally ..." % seconds)
    time.sleep(seconds)
    console("Trace.Stop")
    console("stat namedevents")                 # a toggle: back off (it costs a little on its own)
    time.sleep(3)
    d = os.path.join(OUT, datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + "_trace")
    os.makedirs(d)
    found = adb("shell", "find %s -name '%s' 2>/dev/null" % (SAVED, name)).splitlines()
    if not found:
        sys.exit("no trace file on the headset under " + SAVED)
    local = os.path.join(d, name)
    adb("pull", found[0], local)
    stats = os.path.join(d, "gamethread_timers.csv")
    # (no quotes inside: the whole argument is already quoted, and nested quotes left Insights with no usable path)
    export = 'TimingInsights.ExportTimerStatistics %s -threads=GameThread -columns=*' % stats
    print("exporting with Unreal Insights ...")
    subprocess.run([INSIGHTS, "-OpenTraceFile=%s" % local, "-ExecOnAnalysisCompleteCmd=%s" % export, "-AutoQuit", "-NoUI"],
                   capture_output=True, timeout=900)
    if not os.path.exists(stats):
        sys.exit("Insights wrote nothing; open %s in UnrealInsights.exe by hand" % local)
    rows = list(csv.DictReader(open(stats, encoding="utf-8", errors="replace")))
    keys = list(rows[0].keys()) if rows else []
    key_ex = next((k for k in keys if k.lower().startswith("excl")), None)
    key_in = next((k for k in keys if k.lower().startswith("incl")), None)
    out = ["Game thread, %d s trace (%s): costliest by exclusive time (columns: %s)" % (seconds, name, ", ".join(keys)),
           "%12s %12s %9s   %s" % ("excl ms", "incl ms", "count", "name")]
    for r in sorted(rows, key=lambda r: -(fnum(r.get(key_ex)) or 0))[:60]:
        out.append("%12.1f %12.1f %9s   %s" % ((fnum(r.get(key_ex)) or 0) * 1000, (fnum(r.get(key_in)) or 0) * 1000,
                                             r.get("Count", ""), r.get("Name", "")))
    open(os.path.join(d, "trace_report.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("\n".join(out[:45]))
    print("\ntrace:", local, "(open it in UnrealInsights.exe for the timeline)")


def newest_session():
    ds = sorted(glob.glob(os.path.join(OUT, "2*")))
    if not ds:
        sys.exit("no sessions in " + OUT)
    return ds[-1]


def is_quest(serial):
    make = adb("-s", serial, "shell", "getprop", "ro.product.manufacturer").lower()
    return make in ("oculus", "meta") or "quest" in adb("-s", serial, "shell", "getprop", "ro.product.model").lower()


def pick_device():
    """One headset for every adb call (and the helpers this starts): USB when plugged in, else Wi-Fi (quest_wifi.py),
    reconnecting to the saved Wi-Fi address when no Quest is connected.  Both at once would make plain adb refuse.
    Only a Quest: another Android device on adb (a TV box over the network, say) must never be the one picked."""
    if os.environ.get("ANDROID_SERIAL"):
        return
    def ready():
        rows = [l.split() for l in adb("devices").splitlines()[1:] if len(l.split()) >= 2]
        return [r[0] for r in rows if r[1] == "device" and is_quest(r[0])]
    serials = ready()
    ip_file = os.path.join(OUT, "quest_ip.txt")
    if not serials and os.path.exists(ip_file):
        adb("connect", "%s:5555" % open(ip_file).read().strip())
        serials = ready()
    if serials:
        usb = [s for s in serials if ":" not in s]
        os.environ["ANDROID_SERIAL"] = (usb or serials)[0]


def require_quest():
    if not os.environ.get("ANDROID_SERIAL") or adb("get-state") != "device":
        others = [l.split()[0] for l in adb("devices").splitlines()[1:] if l.strip()]
        print("no Quest on adb%s (plug it in and allow USB debugging, or `quest_wifi.py setup` / `connect` for Wi-Fi)" % (
            " (only: %s)" % ", ".join(others) if others else ""))
        return False
    return True


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    c = sys.argv[1]
    offline = "--fake" in sys.argv or (c == "auto" and sys.argv[2:3] == ["stop"])
    if c not in ("_gpu", "report", "compare") and not offline:
        pick_device()
        if not require_quest():
            sys.exit(1)
    if c == "_gpu":
        gpu_sidecar(sys.argv[2])
    elif c in ("auto", "compare"):
        import questperf_auto
        questperf_auto.main(sys.modules[__name__], sys.argv[1:])
    elif c == "record":
        cmd_record()
    elif c == "stop":
        cmd_stop()
    elif c == "report":
        d = sys.argv[2] if len(sys.argv) > 2 else newest_session()
        if os.path.exists(os.path.join(d, "capture.json")):
            import questperf_auto                  # (an auto capture's folder: its own report, the headline first)
            questperf_auto.redo(sys.modules[__name__], d)
        else:
            report(d)
    elif c == "pull":
        d = os.path.join(OUT, datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + "_pull")
        pull(d)
        report(d)
    elif c == "trace":
        cmd_trace(int(sys.argv[2]) if len(sys.argv) > 2 else 30)
    elif c == "capture" and len(sys.argv) > 2 and sys.argv[2] in ("on", "off"):
        cmd_capture(sys.argv[2] == "on")
    elif c == "mark":
        print("marked" if console("ol.perf.mark") else "the game did not take it (is it running?)")
    elif c == "cmd":
        print("sent" if console(" ".join(sys.argv[2:])) else "the game did not take it (is it running?)")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
