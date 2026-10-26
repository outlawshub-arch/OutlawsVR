"""CPU2: where the game thread's time goes, from a pulled Saved/Perf folder (perf_timeline.csv, perf_buckets.csv,
perf_counts.csv, hitches.csv).

  python cpu2_buckets.py <Perf dir> [level] [t0 t1 label ...]

With no windows: the whole of the level's live play, plus its first and last 2 minutes.  The old split (our buckets
and the untracked rest) prints as before; the CPU2 split (the "Eng:" phase buckets, 2026-10-05) when the run has them:
  world start      OnWorldTickStart -> OnWorldPreActorTick: the XR runtime's xrWaitFrame, net tick, navigation
  actor ticks      every tick group; less our buckets and the bad guys' CharacterMovement = engine tick work
                   (physics, other components, tick-group overhead)
  world end        timers, tickables, cameras, streaming, garbage collection
  outside world    game_ms less the world's tick: Slate, the viewport's draw, end-of-frame work
"""
import csv, statistics as st, sys, os
from collections import defaultdict

OURS = ['Enemies (all)', 'Player tick (total)', 'Level tick (total)', 'Sprites (all)', 'Movie screen']
TOP = OURS + ['INF runtime', 'Level: INF run', 'Level: visibility', 'Player: guns', 'Player: hud', 'Player: hands']
CMC = 'Enemies: movement (CMC)'
OTHER = 'Other ours'
ENG_START = 'Eng: world start (XR wait net)'  # older builds: 'XR wait, net' (normalised in load)
ENG_ACT = 'Eng: actor ticks (total)'
ENG_END = 'Eng: world end (timers GC streaming)'  # older builds had commas
ENG_ALL = 'Eng: world tick (total)'
ENG_GC = 'Eng: GC'


def pct(v, p):
    v = sorted(v)
    if not v: return float('nan')
    k = (len(v) - 1) * p / 100; f = int(k); c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def load(D, level):
    rd = lambda n: list(csv.DictReader(open(os.path.join(D, n), encoding='utf-8'))) if os.path.exists(os.path.join(D, n)) else []
    T = rd('perf_timeline.csv')
    if not level:
        cnt = defaultdict(int)
        for r in T:
            if r['in_play'] == '1': cnt[r['level']] += 1
        level = max(cnt, key=cnt.get) if cnt else ''
    T = [r for r in T if r['level'] == level and r['in_play'] == '1']
    B = defaultdict(dict)
    for r in rd('perf_buckets.csv'):
        if r['level'] == level: B[float(r['t_s'])][r['bucket'].replace(';', '').replace(',', '')] = float(r['avg_ms'])  # names without commas (older builds wrote ; or ,)
    C = {float(r['t_s']): r for r in rd('perf_counts.csv') if r['level'] == level}
    H = [float(r['t_s']) for r in rd('hitches.csv') if r['level'] == level and r.get('kind', 'hitch') == 'hitch']
    return level, T, B, C, H


def win(T, B, C, H, a, b, label):
    rows = [r for r in T if a <= float(r['t_s']) < b]
    if not rows: return None
    t0 = float(rows[0]['t_s']); t1 = float(rows[-1]['t_s']); dur = t1 - t0 + 1
    g = lambda k: [float(r[k]) for r in rows]
    fr = g('frame_ms'); nh = sum(1 for h in H if t0 - 1 < h <= t1)
    print(f'## {label}: t {t0:.0f}-{t1:.0f} s ({dur/60:.1f} min, {len(rows)} 1-s rows)')
    print(f' frame med {st.median(fr):.1f} p95 {pct(fr,95):.1f} p99 {pct(fr,99):.1f} | fps med {st.median(g("fps")):.1f} | per-second worst frame med {st.median(g("frame_max_ms")):.1f} | hitches {nh} = {nh/(dur/60):.1f}/min')
    print(' game {:.1f} render {:.1f} rhi {:.1f} gpu {:.1f} (medians); draws {:.0f} tris {:.0f}'.format(*[st.median(g(k)) for k in ['game_ms', 'render_ms', 'rhi_ms', 'gpu_ms', 'draws', 'tris']]))
    bk = lambda r, k: B.get(float(r['t_s']), {}).get(k, 0.0)
    out = []
    for k in TOP:
        v = [B[float(r['t_s'])].get(k) for r in rows]; v = [x for x in v if x is not None]
        if v: out.append(f'{k} {st.median(v):.2f}/{pct(v,95):.2f}')
    unt = [float(r['game_ms']) - sum(bk(r, k) for k in OURS) for r in rows]
    print(' buckets med/p95:', '; '.join(out), f'; untracked game {st.median(unt):.2f}/{pct(unt,95):.2f}')
    if any(ENG_ALL in B.get(float(r['t_s']), {}) for r in rows):
        split = defaultdict(list)
        for r in rows:
            game = float(r['game_ms'])
            ours = sum(bk(r, k) for k in OURS) + bk(r, OTHER)
            split['ours (all buckets)'].append(ours)
            split['  of it: other ours (coop, beacon, stats, remote players)'].append(bk(r, OTHER))
            split['enemy movement (CMC)'].append(bk(r, CMC))
            split['world start (XR wait, net)'].append(bk(r, ENG_START))
            split['engine actor-tick rest (physics, components)'].append(bk(r, ENG_ACT) - ours - bk(r, CMC))
            split['world end (timers, tickables, GC)'].append(bk(r, ENG_END))
            split['  of it: GC'].append(bk(r, ENG_GC))
            split['outside the world tick (Slate, draw, sync)'].append(game - bk(r, ENG_ALL))
        print(' CPU2 split of game_ms, med / p95 / mean ms:')
        for k, v in split.items():
            print(f'   {k:58s} {st.median(v):6.2f} {pct(v,95):6.2f} {st.fmean(v):6.2f}')
        gcs = [int(C[float(r['t_s'])].get('gcs') or 0) for r in rows if float(r['t_s']) in C]
        if gcs: print(f'   garbage collections: {sum(gcs)} ({sum(gcs)/(dur/60):.1f}/min)')
    ce = [C[float(r['t_s'])] for r in rows if float(r['t_s']) in C]
    if ce: print(' enemies (counts.csv) start {} end {}, awake med {:.0f}'.format(ce[0]['enemies'], ce[-1]['enemies'], st.median([float(c['enemies_awake']) for c in ce])))
    return t0, t1


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    D = sys.argv[1]
    level = sys.argv[2] if len(sys.argv) > 2 else ''
    level, T, B, C, H = load(D, level)
    print(f'level {level}')
    rest = sys.argv[3:]
    wins = [(float(rest[i]), float(rest[i + 1]), rest[i + 2]) for i in range(0, len(rest) - 2, 3)]
    if not wins and T:
        wins = [(float(T[0]['t_s']), float(T[-1]['t_s']) + 1, 'live play')]
    for a, b, l in wins:
        r = win(T, B, C, H, a, b, l)
        if r and r[1] - r[0] > 300:
            win(T, B, C, H, r[0], r[0] + 120, l + ' first 2 min'); win(T, B, C, H, r[1] - 119, r[1] + 1, l + ' last 2 min')


if __name__ == '__main__':
    main()
