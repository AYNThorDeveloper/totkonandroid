#!/usr/bin/env python3
"""Table of a thor_ab.sh run: per env, the 300-frame timing blocks inside the measurement window (medians over blocks).

  frame p50/p95   presented frame time (the 30 fps cap makes ~33.3 the floor)
  work p50        the frame's own work without the cap's wait ("出帧工作")
  nvn p50         submit + present on our side (translation when synchronous)
  gpu p50/p95     GPU span per frame
  passes, segs    render passes and extra segments per frame
  copies          uniform copies per frame (TOTK_VK_UBO_COPY / async capture)
  long>50         frames above 50 ms in the window (freeze lines)
usage: thor_ab_report.py <out dir>
"""
import os
import re
import statistics
import sys

TIMING = re.compile(r'帧时间 p50 ([\d.]+) / p95 ([\d.]+).*?出帧工作 p50 ([\d.]+).*?NVN 提交\+出图 p50 ([\d.]+).*?GPU p50 ([\d.]+) / p95 ([\d.]+)')
PASSES = re.compile(r'([\d.]+) passes, (\d+) draws per frame')
SEGS = re.compile(r'([\d.]+) extra segments per frame')
COPIES = re.compile(r'uniform copies per frame (\d+)')
FREEZE = re.compile(r'freeze: frame \d+ took (\d+) ms')


def med(v):
    return statistics.median(v) if v else float('nan')


def one(run):
    path = os.path.join(run, 'run.log')
    if not os.path.exists(path):
        return None
    lines = open(path, errors='replace').read().splitlines()
    start = 0
    mf = os.path.join(run, 'measure_from_line')
    if os.path.exists(mf):
        start = int(open(mf).read().strip() or 0)
    else:
        return dict(state='not in game')
    w = lines[start:]
    t = [m.groups() for m in map(TIMING.search, w) if m]
    r = dict(state='ok' if t else 'no timing lines')
    if t:
        col = lambda i: med([float(x[i]) for x in t])
        r.update(frame50=col(0), frame95=col(1), work50=col(2), nvn50=col(3), gpu50=col(4), gpu95=col(5))
    p = [m.groups() for m in map(PASSES.search, w) if m]
    r['passes'] = med([float(x[0]) for x in p]); r['draws'] = med([float(x[1]) for x in p])
    r['segs'] = med([float(m.group(1)) for m in map(SEGS.search, w) if m])
    r['copies'] = med([float(m.group(1)) for m in map(COPIES.search, w) if m])
    r['long50'] = sum(1 for m in map(FREEZE.search, w) if m and int(m.group(1)) > 50)
    if any('fatal signal' in l.lower() or 'nvk_die' in l or 'FAIL:' in l for l in lines):
        r['state'] = 'CRASH/FAIL'
    return r


def main():
    out = sys.argv[1]
    runs = sorted(d for d in os.listdir(out) if os.path.isdir(os.path.join(out, d)))
    hdr = ('env', 'frame p50', 'p95', 'work', 'nvn', 'gpu p50', 'p95', 'passes', 'draws', 'segs', 'copies', 'long>50', 'state')
    print('| ' + ' | '.join(hdr) + ' |')
    print('|' + '---|' * len(hdr))
    f = lambda v: '-' if v != v else ('%.1f' % v)
    for d in runs:
        r = one(os.path.join(out, d))
        if r is None:
            continue
        print('| ' + ' | '.join([d] + [f(r.get(k, float('nan'))) for k in ('frame50', 'frame95', 'work50', 'nvn50', 'gpu50', 'gpu95', 'passes', 'draws', 'segs', 'copies')]
                                + [str(r.get('long50', '-')), r['state']]) + ' |')


if __name__ == '__main__':
    main()
