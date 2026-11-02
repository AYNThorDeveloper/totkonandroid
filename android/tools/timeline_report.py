#!/usr/bin/env python3
"""Per-frame chain from a TOTK_TIMELINE file: who waits for whom.

Run on the Thor with TOTK_TIMELINE=/data/local/tmp/totk-android-v143/timeline.tsv:<first frame>:<frames> in the env,
pull the file, then: timeline_report.py timeline.tsv [--frames N]

Per frame (frames are counted by the game's presents):
  interval   present to present
  submit     time inside nvnQueueSubmitCommands (the game thread is blocked; with TOTK_ASYNC_REPLAY only the hand-over)
  replay     submit_exec (translation), on the game thread or the translation thread
  gpu        GPU span of the frame's submission (first segment start to end)
  gpu_lag    GPU start minus replay start (how soon the GPU gets work)
  tail       GPU end minus replay end (GPU work left once the translation is done)
  idle       interval minus GPU span: time the GPU had nothing of this frame
  waits      the game blocked in nvnSyncWait / nvnWindowAcquireTexture
Then medians and 95th percentiles, and which link is the longest most often: that link sets the frame time once the
others overlap it (full overlap: interval = the longest link).
"""
import argparse
import collections
import statistics
import sys


def load(path):
    ev = []
    with open(path) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            t, kind, tid, frame, a, b = line.rstrip('\n').split('\t')
            ev.append((int(t), kind, int(tid), int(frame), int(a), int(b)))
    ev.sort()
    return ev


def pct(v, p):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file')
    ap.add_argument('--frames', type=int, default=0, help='only the first N complete frames')
    ap.add_argument('--from-frame', type=int, default=0, help='skip frames before this game frame (loading screens)')
    a = ap.parse_args()
    ev = load(a.file)
    pres = [e for e in ev if e[1] == 'pres_in']
    if len(pres) < 3:
        sys.exit('fewer than 3 presents in the file')
    open_call = {}
    calls = collections.defaultdict(list)   # kind -> [(start, end, tid)]
    for t, kind, tid, frame, x, y in ev:
        for name in ('sub', 'rep', 'wait', 'acq', 'finish'):
            if kind == name + ('_in' if name != 'rep' else '_begin'):
                open_call[(name, tid)] = t
            elif kind == name + ('_out' if name != 'rep' else '_end') and (name, tid) in open_call:
                calls[name].append((open_call.pop((name, tid)), t, tid))
    gpu = [(t, y, x) for t, kind, tid, frame, x, y in ev if kind == 'gpu']   # (start, end, fence)
    fence_of_commit = {x: y for t, kind, tid, frame, x, y in ev if kind == 'fence'}
    rows = []
    for i in range(1, len(pres)):
        if pres[i - 1][3] < a.from_frame:
            continue
        lo, hi = pres[i - 1][0], pres[i][0]
        inside = lambda c: [s for s in c if lo <= s[0] < hi]
        subs, reps = inside(calls['sub']), inside(calls['rep'])
        waits = sum(e - s for s, e, _ in inside(calls['wait'])) + sum(e - s for s, e, _ in inside(calls['acq']))
        sub_t = sum(e - s for s, e, _ in subs)
        rep_t = sum(e - s for s, e, _ in reps)
        rep_lo = min((s for s, _, _ in reps), default=None)
        rep_hi = max((e for _, e, _ in reps), default=None)
        g = [x for x in gpu if rep_lo is not None and x[0] >= rep_lo - 5_000_000 and x[0] < hi]
        g_lo = min((s for s, _, _ in g), default=None)
        g_hi = max((e for _, e, _ in g), default=None)
        gspan = (g_hi - g_lo) if g else 0
        rows.append(dict(interval=hi - lo, submit=sub_t, replay=rep_t, gpu=gspan,
                         gpu_lag=(g_lo - rep_lo) if g and rep_lo is not None else None,
                         tail=(g_hi - rep_hi) if g and rep_hi is not None else None,
                         idle=max(0, (hi - lo) - gspan), waits=waits))
        if a.frames and len(rows) >= a.frames:
            break
    ms = lambda v: v / 1e6
    keys = ['interval', 'submit', 'replay', 'gpu', 'gpu_lag', 'tail', 'idle', 'waits']
    print('%-9s %8s %8s' % ('ms', 'p50', 'p95'))
    for k in keys:
        v = [r[k] for r in rows if r[k] is not None]
        print('%-9s %8.2f %8.2f' % (k, ms(pct(v, 50)), ms(pct(v, 95))))
    longest = collections.Counter(max(('replay', 'gpu', 'game'), key=lambda k: r[k] if k != 'game' else r['interval'] - r['submit'] - r['waits']) for r in rows)
    print('\nlongest link per frame:', ', '.join('%s %d' % kv for kv in longest.most_common()))
    print('frames: %d; mean interval %.2f ms; with full overlap the interval would approach max(game, replay, GPU)' % (len(rows), ms(statistics.mean(r['interval'] for r in rows))))
    if not gpu:
        print('(no gpu events: GPU timestamps off, or a build without them)')
    if not fence_of_commit:
        print('(no fence events)')


if __name__ == '__main__':
    main()
