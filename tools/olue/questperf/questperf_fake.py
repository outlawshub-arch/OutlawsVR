"""A stand-in Quest for questperf auto (REQ-QUESTPERFRUN): no headset time for testing the auto mode.

  python questperf_fake.py build <a real Shipping OutlawsUE.log> <feed.json> [--shuffle] [--pre=SECONDS] [--hz=120 [--fallback]] [--drop] [--perftest]
  python questperf.py auto --fake <feed.json> --out <dir>

build: the real log's lines from its start to the level after The Town, at their own times, then a scripted part: a quit
to the menu, the menu showing The Town (not a start), a load of The Town with a Graphics change, a death and its respawn,
the headset off for 90 s, Restart Level twice, the level complete, the game closed.  --shuffle: the randomizer's mission
order shuffled, so the auto mode has to learn level 2's map from the run's lines.  --pre=S: the game S seconds in when the
auto mode arms (a level under way is not a start).  --hz=N: the Refresh Rate row and the log at N Hz; --fallback: the
compositor runs 72 Hz all the same (a headset that fell back).  --drop: adb loses the headset during the 90 s pause.
--perftest: then the hidden Performance test row's run (its own capture and settings dump, a death, the level's exit).
FakeQuest plays the feed a second at a time (as fast as it can): the game's log file, Saved/Perf/capture.on picked up
within a second and a capture_<time>.csv written a row a frame (in_play from the level's state: the menu, a death, a
pause, a load or a movie are not play), perf_timeline.csv and perf_counts.csv a row a second, a compositor line (VrApi)
a second, and the GPU counters.  Its frame costs follow the settings (resolution, lamps, HD art) and the bad guys awake.
"""
import datetime
import json
import math
import os
import random
import re
import shutil
import sys

DEVICE_SAVED = "/storage/emulated/0/Android/data/com.outlawsvr.OutlawsUE/files/UnrealGame/OutlawsUE/OutlawsUE/Saved"
CAP_HEAD = ("t_s,level,frame_ms,game_ms,render_ms,rhi_ms,gpu_ms,draws,tris,in_play,load,xr_wait_ms,gc_ms,textures_made,"
            "Enemies: think,Player: guns")
TL_HEAD = ("t_s,level,x,y,z,yaw,fps,frame_ms,frame_max_ms,game_ms,render_ms,rhi_ms,gpu_ms,gpu_max_ms,draws,tris,mem_mb,"
           "textures_made,mtexels_made,top_bucket,top_bucket_ms,in_play")
CN_HEAD = ("t_s,level,actors,actors_ticking,sprites,sprites_shown,sprites_ticking,enemies,enemies_awake,meshes,meshes_shown,"
           "sections_shown,sectors_visible,mover_groups,gcs,throttle_level,throttle_game_ms,throttle_xr_wait_ms,throttle_eff_ms")


class FakeQuest:
    def __init__(self, feed_path):
        F = json.load(open(feed_path, encoding="utf-8"))
        self.events = sorted(F["events"], key=lambda e: e[0])
        self.ei = 0
        self.settings = dict(F.get("settings", {}))
        self.root = os.path.join(os.path.dirname(os.path.abspath(feed_path)), "fake_device")
        shutil.rmtree(self.root, ignore_errors=True)
        for sub in ("Logs", "Perf"):
            os.makedirs(os.path.join(self.root, sub))
        self.logf = os.path.join(self.root, "Logs", "OutlawsUE.log")
        self.t0 = float(F.get("start", 1793000000.0))
        self.clock = 0.0
        self.end_t = self.events[-1][0] + 10 if self.events else 10
        self.pid, self.next_pid = "", 4100
        self.sink = None
        self.gpu = {}
        self.rng = random.Random(7)
        self.session0 = 0.0
        self.level, self.mode, self.mode_until, self.mode_back = None, "none", 0.0, "play"
        self.level_t0, self.attempts = 0.0, {}
        self.cap, self.cap_frames, self.cap_t0 = None, 0, 0.0
        self.paused_until, self.resumes = -1.0, 0
        self.temp = 33.0
        self.pending_cap = []
        self.compositor_hz = F.get("compositor_hz")          # (a headset that fell back from the rate asked)
        self.drop_until = -1.0                               # (adb over Wi-Fi gone: the headset asleep)
        self.test_on = False                                 # (REQ-PERFTESTMODE: the game's own capture, by itself)
        if F.get("pre_roll"):
            self.advance(float(F["pre_roll"]))

    # ---- the interface questperf_auto.Auto uses
    def now(self):
        return self.t0 + self.clock

    def tick(self):
        self.advance(1.0)

    def sleep(self, s):
        self.advance(s)

    def finished(self):
        return self.clock >= self.end_t

    def log_state(self):
        if self.clock < self.drop_until:
            return None
        if not os.path.exists(self.logf):
            return self.pid, -1, ""
        head = open(self.logf, "rb").read(40).decode("utf-8", "replace")
        return self.pid, os.path.getsize(self.logf), head.strip()

    def read_log(self, offset):
        with open(self.logf, "rb") as f:
            f.seek(offset)
            return f.read()

    def settings_text(self):
        return "; Outlaws VR settings (the in-game menu writes this)\n" + "".join("%s=%s\n" % kv for kv in sorted(self.settings.items()))

    def version(self):
        return "46", "1.0-fake"

    def device(self):
        return {"model": "Quest 3", "make": "Oculus", "serial": "FAKE0001"}

    def battery(self):
        return {"level": "81", "temperature": str(int(self.temp * 10 - 40)), "status": "3", "AC powered": "false", "USB powered": "false"}

    def capture_flag(self, on):
        p = os.path.join(self.root, "Perf", "capture.on")
        if self.clock < self.drop_until:
            return os.path.exists(p)                         # (out of reach: nothing changes on the headset)
        if on:
            open(p, "w").close()
        elif os.path.exists(p):
            os.remove(p)
        return os.path.exists(p)

    def captures(self):
        return sorted(f for f in os.listdir(os.path.join(self.root, "Perf")) if re.match(r"capture_\d{8}_\d{6}\.csv$", f))

    def pull(self, name, local):
        if self.clock < self.drop_until:
            return False
        p = os.path.join(self.root, "Perf", name)
        if not os.path.exists(p):
            return False
        shutil.copyfile(p, local)
        return True

    def start_gpu(self, path):
        self.gpu[path] = open(path, "w", encoding="utf-8")
        return path

    def stop_gpu(self, h):
        f = self.gpu.pop(h, None)
        if f:
            f.close()

    def start_logcat(self, sink):
        self.sink = sink

    def stop_logcat(self):
        self.sink = None

    # ---- the simulation
    def advance(self, dt):
        steps = max(1, int(round(dt)))
        for _ in range(steps):
            t1 = self.clock + 1.0
            while self.ei < len(self.events) and self.events[self.ei][0] <= t1:
                self.event(self.events[self.ei])
                self.ei += 1
            self.clock = t1
            self.second()

    def stamp(self, t):
        u = datetime.datetime.utcfromtimestamp(self.t0 + t)
        return "[%s:%03d][%3d]" % (u.strftime("%Y.%m.%d-%H.%M.%S"), u.microsecond // 1000, int(t * 72) % 1000)

    def write(self, t, text):
        if not self.pid:
            return
        with open(self.logf, "a", encoding="utf-8", newline="\n") as f:
            f.write(self.stamp(t) + text + "\n")

    def event(self, e):
        t, kind = e[0], e[1]
        if kind == "launch":
            if os.path.exists(self.logf):
                os.replace(self.logf, os.path.join(self.root, "Logs", "OutlawsUE-backup-%d.log" % int(t)))
            self.pid = str(self.next_pid)
            self.next_pid += 7
            self.session0 = t
            self.mode = "menu"
            self.cap = None
            with open(self.logf, "w", encoding="utf-8", newline="\n") as f:
                f.write("﻿Log file open, %s\n" % datetime.datetime.fromtimestamp(self.t0 + t).strftime("%Y.%m.%d-%H.%M.%S"))
                f.write("(Shipping: the game's own Log, Display, Warning and Error lines; the engine's lines are not in a Shipping build)\n")
        elif kind == "exit":
            self.pid = ""
            self.cap = None
        elif kind == "set":
            self.settings[e[2]] = e[3]
        elif kind == "drop":
            self.drop_until = t + e[2]
        elif kind == "pause":
            self.paused_until = t + e[2]
            self.mode_back_pause = self.mode
        elif kind == "log":
            text = e[2]
            if "{CAP}" in text:
                stamp = datetime.datetime.fromtimestamp(self.t0 + t).strftime("%Y%m%d_%H%M%S")
                cap, sett = "capture_%s.csv" % stamp, "perftest_%s_settings.txt" % stamp
                open(os.path.join(self.root, "Perf", sett), "w").write("build: 73ab7c0\nrefresh in effect: %s Hz\n" % self.settings.get("RefreshRate"))
                self.test_on, self.test_name = True, cap
                text = text.replace("{CAP}", "%s/Perf/%s" % (DEVICE_SAVED, cap)).replace("{SET}", "%s/Perf/%s" % (DEVICE_SAVED, sett))
            if "OLPERF test end" in text:
                self.test_on = False
            self.write(t, text)
            body = text.split(": ", 1)[-1]
            if body.startswith("OLRT level restart") or body.startswith("OLRANDO load:"):
                self.mode = "load"
            m = re.match(r"OLRT level load: (\S+)", body)
            if m:
                self.level = m.group(1)
                self.level_t0 = t
                self.attempts[self.level] = self.attempts.get(self.level, -1) + 1
                self.mode = "play"
            if body.startswith("OLRT main menu level"):
                self.mode = "menu"
            if body.startswith("OLRT player died"):
                self.mode = "dead"
            if body.startswith("OLRT level complete"):
                self.mode = "movie"
            if body.startswith("OLRT pause page rows") and self.mode == "play":
                self.mode, self.mode_until, self.mode_back = "pausemenu", t + 4, "play"

    def cost(self, lt, att):
        s = self.settings
        scale = float(s.get("RenderScale", "100")) / 100.0
        lamps = 4 if s.get("LampLights", "auto") == "auto" else int(s["LampLights"])
        hd = s.get("RemasteredGraphics", "true") == "true"
        awake = max(0, int(round(5 + 4 * math.sin(lt / 37.0 + att) + self.rng.uniform(-1, 1))))
        draws = int(220 + 18 * awake + 40 * math.sin(lt / 20.0))
        return {"gpu": 6.4 * scale * scale + 0.45 * lamps + (0.8 if hd else 0), "game": 3.1 + 0.42 * awake, "render": 2.0 + 0.004 * draws,
                "rhi": 1.4 + 0.002 * draws, "draws": draws, "tris": 120000 + 6000 * awake, "awake": awake}

    def second(self):
        if not self.pid:
            return
        t = self.clock
        if t < self.paused_until:
            return
        if self.paused_until > 0 and t >= self.paused_until:
            self.paused_until = -1
            self.resumes += 1
            self.write(t, "LogOutlaws: OLRT resume %d (app resumed): before: refresh 72 Hz (asked 72), vr.PixelDensity 1.20" % self.resumes)
        if self.mode == "pausemenu" and t >= self.mode_until:
            self.mode = self.mode_back
        flag = os.path.exists(os.path.join(self.root, "Perf", "capture.on")) or self.test_on
        gs = t - self.session0
        if flag and not self.cap:
            name = self.test_name if self.test_on else "capture_%s.csv" % datetime.datetime.fromtimestamp(self.t0 + t).strftime("%Y%m%d_%H%M%S")
            self.cap, self.cap_frames, self.cap_t0 = os.path.join(self.root, "Perf", name), 0, t
            open(self.cap, "w", encoding="utf-8").write(CAP_HEAD + "\n")
            self.write(t, "LogOutlaws: OLPERF capture started: %s/Perf/%s" % (DEVICE_SAVED, name))
        elif not flag and self.cap:
            open(self.cap, "a", encoding="utf-8").write(CAP_HEAD + "\n")
            self.write(t, "LogOutlaws: OLPERF capture stopped: %d frames over %.0f s in %s/Perf/%s" % (
                self.cap_frames, t - self.cap_t0, DEVICE_SAVED, os.path.basename(self.cap)))
            self.cap = None
        hz = int(self.compositor_hz or self.settings.get("RefreshRate", "72"))
        budget = 1000.0 / hz
        lt = t - self.level_t0
        att = self.attempts.get(self.level, 0)
        c = self.cost(lt, att)
        play = self.mode == "play"
        rows, over, fsum, fmax = [], 0, 0.0, 0.0
        for k in range(hz):
            j = 1 + self.rng.gauss(0, 0.07)
            work = max(c["game"], c["render"], c["gpu"]) * j + (22.0 if self.rng.random() < 0.004 else 0.0)
            if not play:
                work = budget * 0.6
            frame = budget if work <= budget else budget * math.ceil(work / budget)
            if frame > budget * 1.05:
                over += 1
            fsum += frame
            fmax = max(fmax, frame)
            if self.cap:
                rows.append("%.3f,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%.2f,%.2f,0,%.3f,%.3f" % (
                    gs + k / hz, self.level or "", frame, c["game"] * j, c["render"] * j, c["rhi"] * j, max(c["gpu"] * j, frame - 1), c["draws"], c["tris"],
                    1 if play else 0, 1 if self.mode == "load" else 0, max(0.0, budget - work), 0.0, 0.08 * c["awake"], 0.3))
        if self.cap:
            open(self.cap, "a", encoding="utf-8").write("\n".join(rows) + "\n")
            self.cap_frames += len(rows)
        # where the player is: the same route each attempt, a little to one side (the runs go roughly the same way)
        x = 1000 + 22 * lt + 150 * (att % 2)
        y = 2000 + 1400 * math.sin(lt / 45.0)
        tl = os.path.join(self.root, "Perf", "perf_timeline.csv")
        cn = os.path.join(self.root, "Perf", "perf_counts.csv")
        if not os.path.exists(tl):
            open(tl, "w").write(TL_HEAD + "\n")
            open(cn, "w").write(CN_HEAD + "\n")
        open(tl, "a").write("%.1f,%s,%.0f,%.0f,110,90,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%.0f,0,0.00,Enemies: think,%.3f,%d\n" % (
            gs, self.level or "", x, y, 1000.0 * hz / fsum, fsum / hz, fmax, c["game"], c["render"], c["rhi"], c["gpu"], c["gpu"] * 1.3, c["draws"],
            c["tris"], 1600 + 3 * c["awake"], 0.08 * c["awake"], 1 if play else 0))
        open(cn, "a").write("%.1f,%s,500,110,380,80,0,90,%d,400,150,370,12,55,0,0,0.00,0.00,0.00\n" % (gs, self.level or "", c["awake"] if play else 0))
        self.temp = min(46.0, self.temp + (0.03 if play else 0.01))
        if self.sink and self.clock >= self.drop_until:
            u = datetime.datetime.fromtimestamp(self.t0 + t)
            self.sink(self.now(), "%s I/VrApi   (%5s): FPS=%d/%d,Prd=37ms,Tear=0,Early=0,Stale=%d,Stale2/5/10/max=0/0/0/0,VSnc=1,Lat=-4,Fov=0,"
                      "CPU4/GPU=3/3,2208/525MHz,OC=3F,TA=0/0/0,SP=N/N/N,Mem=3196MHz,Free=%dMB,PLS=0,Temp=%.1fC/0.0C,TW=1.2ms,App=%.2fms,"
                      "GD=0.00ms,CPU&GPU=20.1ms,LCnt=6(DR0,LM0),GPU%%=%.2f,CPU%%=%.2f(W0.5)" % (
                          u.strftime("%m-%d %H:%M:%S.") + "%03d" % (u.microsecond // 1000), self.pid, hz - over, hz, over, 1400 - 2 * c["awake"],
                          self.temp, c["gpu"], min(0.99, c["gpu"] / budget), min(0.99, c["game"] / budget)))
        for f in self.gpu.values():
            f.write(json.dumps({"t": self.now(), "m": {"GPU % Utilization": min(99.0, 100 * c["gpu"] / budget) if play else 20.0,
                                                       "% Time Shading Fragments": 97.5, "GPU % Bus Busy": 30.0 + c["gpu"],
                                                       "% Texture Fetch Stall": 2.5}}) + "\n")
            f.flush()


# ------------------------------------------------------------------------------------------------ the feed

def parse_stamp(line):
    m = re.match(r"^\[(\d{4})\.(\d\d)\.(\d\d)-(\d\d)\.(\d\d)\.(\d\d):(\d{3})\]\[\s*\d+\](.*)$", line)
    if not m:
        return None, None
    y, mo, d, h, mi, s, ms = (int(x) for x in m.groups()[:7])
    return datetime.datetime(y, mo, d, h, mi, s, ms * 1000).timestamp(), m.group(8)


def build(real_log, out, shuffle=False, pre_roll=0, hz=72, fallback=False, drop=False, perftest=False):
    lines = open(real_log, encoding="utf-8-sig", errors="replace").read().splitlines()
    ev, t_first, t_end, after = [], None, None, None
    for l in lines:
        ts, text = parse_stamp(l)
        if ts is None:
            continue
        if t_first is None:
            t_first = ts
        t = ts - t_first + 2.0
        if hz != 72:
            text = text.replace("refresh 72 Hz asked", "refresh %d Hz asked" % hz).replace("refresh 72 Hz (asked 72)", "refresh %d Hz (asked %d)" % (hz, hz))
        if shuffle and "OLRANDO run (new game)" in text:
            text = text.replace("mission order off", "mission order shuffle middle")
        if shuffle and "OLRT level complete (boss defeated): movie she_tob, next level townex" in text:
            ev.append([t - 0.01, "log", "LogOutlaws: OLRANDO story: hideout is level 1 of 9 in this run: cutscene she_tob (the story's 1st), next townex"])
        ev.append([t, "log", text])
        if re.search(r"OLRT level load: train ", text):
            after = t
        if after is not None and t > after + 20:
            t_end = t
            break
    if after is None:
        sys.exit("the log has no train load after The Town")
    T = t_end + 10

    def L(dt, text):
        ev.append([T + dt, "log", "LogOutlaws: " + text])
    load = "OLRT level load: townex 4100 ms (textures built 1358, from cache 0, decode ms 1858, mips ms 429, upload ms 435, read ms 226, cache io ms 0; BC7 on the CPU)"
    placed = "OLRT player placed at X=1091.700 Y=2033.400 Z=110.000 yaw 351.1"
    # a quit to the menu
    L(20, "OLRT pause page rows (8, no scroll): Resume | Restart Level | End Game | Options | Save Game | Load Game | Tools | Exit")
    L(25, "OLRT level restart")
    L(28, "OLRT level load: ranch 3600 ms (textures built 1126, from cache 0, decode ms 1708, mips ms 404, upload ms 424, read ms 206, cache io ms 0; BC7 on the CPU)")
    L(28, "OLRT player placed at X=10000.500 Y=12716.400 Z=-331.000 yaw 90.0")
    L(28, "OLRT main menu level ranch: built, 109 bad guy places, the view from place 90")
    # the menu's own view is The Town: not a start
    L(60, "OLRT level restart")
    L(63, load)
    L(63, placed)
    L(63, "OLRT main menu level townex: built, 82 bad guy places, the view from place 12")
    # a load of The Town: capture 2, a Graphics change, a death and its respawn, then the headset off
    L(90, "OLRANDO load: enemies anything, items shuffle places, mission order %s, seed 424242; townex at attempt 0 (its roll as saved)"
      % ("shuffle middle" if shuffle else "off"))
    L(90.2, "OLRT level restart")
    L(95, load)
    L(95, "OLRT rebuilt after two collections: 772 MB used before the build, 1717 after; swap free 3959 MB")
    L(95, placed)
    L(95.05, "OLRT level start: stamina full; right Revolver 4 -> 6 (98 ibullit left)")
    ev.append([T + 140, "set", "RenderScale", "90"])
    ev.append([T + 140, "set", "LampLights", "2"])
    L(140.5, "OLRT graphics (default, menu): resolution 90 %% (vr.PixelDensity 0.90), refresh %d Hz asked, lamp lights 2 (chosen), performance HUD off" % hz)
    L(144, "OLRT eye buffer: render target 1512x1584 (pixel density 0.90, vr.PixelDensity 0.90; the runtime recommends 1680x1760 at 1.0)")
    L(200, "OLSTATS death (deaths 1, by bad guys 1)")
    L(200, "OLRT player died: the view falls from 165 cm")
    L(200, "OLRT player died")
    L(206, "OLRANDO load: enemies anything, items shuffle places, mission order off, seed 424242; townex at attempt 0 (its roll as saved)")
    L(206, "OLRT level restart")
    L(209, load)
    L(209, "OLRT restart with what townex was entered with: 5 items, gold 0, armour 0")
    L(209, placed)
    ev.append([T + 300, "pause", 90])
    if drop:
        ev.append([T + 305, "drop", 85])                   # (the headset asleep: adb over Wi-Fi gone too)
    # Restart Level from the pause menu (no death): capture 3; again: capture 3 ends, capture 4 starts
    L(420, "OLRT pause page rows (8, no scroll): Resume | Restart Level | End Game | Options | Save Game | Load Game | Tools | Exit")
    L(425, "OLRT level restart")
    L(428, load)
    L(428, "OLRT restart with what townex was entered with: 5 items, gold 0, armour 0")
    L(428, placed)
    L(520, "OLRT pause page rows (8, no scroll): Resume | Restart Level | End Game | Options | Save Game | Load Game | Tools | Exit")
    L(524, "OLRT level restart")
    L(527, load)
    L(527, "OLRT restart with what townex was entered with: 5 items, gold 0, armour 0")
    L(527, placed)
    L(640, "OLRT level complete (boss defeated): movie toe_trb, next level train")
    L(646, "OLRT level won: on to train")
    L(646, "OLRT level restart")
    L(650, "OLRT level load: train 3087 ms (textures built 1107, from cache 0, decode ms 1646, mips ms 367, upload ms 395, read ms 250, cache io ms 0; BC7 on the CPU)")
    if perftest:
        # the Performance test row (REQ-PERFTESTMODE) from the main menu: The Town alone, a death in it, the level's exit
        L(660, "OLRT level restart")
        L(663, "OLRT level load: ranch 3600 ms (textures built 1126, from cache 0, decode ms 1708, mips ms 404, upload ms 424, read ms 206, cache io ms 0; BC7 on the CPU)")
        L(663, "OLRT main menu level ranch: built, 109 bad guy places, the view from place 90")
        L(680, "OLRT level restart")
        L(684, load)
        L(684, placed)
        L(684.5, "OLPERF test start: level townex, capture {CAP}, settings {SET}")
        L(750, "OLRT player died")
        L(750, "OLPERF test death 1")
        L(756, "OLRT level restart")
        L(759, load)
        L(759, placed)
        L(820, "OLPERF test end: level exit (9000 frames)")
        L(822, "OLRT level restart")
        L(825, "OLRT level load: ranch 3600 ms (textures built 1126, from cache 0, decode ms 1708, mips ms 404, upload ms 424, read ms 206, cache io ms 0; BC7 on the CPU)")
        L(825, "OLRT main menu level ranch: built, 109 bad guy places, the view from place 90")
    ev.append([T + (900 if perftest else 700), "exit"])
    ev.append([T + (915 if perftest else 715), "end"])
    ev.insert(0, [1.0, "launch"])
    settings = {"RenderScale": "120", "RefreshRate": str(hz), "LampLights": "4", "RemasteredGraphics": "true", "ObjectDetail": "2",
                "StochasticTextures": "true", "Tinting": "true", "Gamma": "1.20", "PerfHud": "false", "GrabGlow": "false",
                "SoloBadGuys": "4.00", "RandoEnemies": "2", "RandoItems": "1", "RandoOrder": "1" if shuffle else "0", "RandoBosses": "1",
                "RandoCritters": "0", "RandoSeed": "424242"}
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump({"start": 1793000000.0, "settings": settings, "events": ev, "pre_roll": pre_roll, "compositor_hz": 72 if fallback else None},
              open(out, "w", encoding="utf-8"))
    print("feed: %d events over %.0f s (the real log to %.0f s, the script after) -> %s" % (len(ev), T + 715, t_end, out))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "build":
        pre = [a for a in sys.argv if a.startswith("--pre=")]
        hz = [a for a in sys.argv if a.startswith("--hz=")]
        build(sys.argv[2], sys.argv[3], "--shuffle" in sys.argv, float(pre[0][6:]) if pre else 0, int(hz[0][5:]) if hz else 72,
              "--fallback" in sys.argv, "--drop" in sys.argv, "--perftest" in sys.argv)
    else:
        sys.exit(__doc__)
