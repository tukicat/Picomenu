# This file is part of picoFace.
#
# picoFace is free software: you can redistribute it and/or modify it under the terms of
# the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
#
# picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with picoFace. If
# not, see <https://www.gnu.org/licenses/>.

# Worst-case stack from GCC -fcallgraph-info=su files: python3 stackdepth.py builddir root [root...]
import os, re, sys, functools
sys.setrecursionlimit(100000)
size, edges, label = {}, {}, {}
def key(t): return t.rsplit(':', 1)[-1] if ':' in t else t
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        if not f.endswith('.ci'): continue
        for line in open(os.path.join(d, f), errors='replace'):
            m = re.match(r'node: \{ title: "([^"]*)" label: "([^"]*)"', line)
            if m:
                k = key(m.group(1)); lab = m.group(2)
                n = re.search(r'\\n(\d+) bytes \((\w[^)]*)\)', lab)
                if n:
                    size[k] = max(size.get(k, 0), int(n.group(1)))
                    label[k] = lab.split('\\n')[0] + ('' if n.group(2) == 'static' else ' [' + n.group(2) + ']')
                continue
            m = re.match(r'edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"', line)
            if m:
                edges.setdefault(key(m.group(1)), set()).add(key(m.group(2)))
onstack = set(); cycles = set(); indirect = set()
@functools.lru_cache(None)
def depth(k):
    if k in onstack: cycles.add(k); return (0, ())
    onstack.add(k)
    best = (0, ())
    for t in edges.get(k, ()):
        if t == '__indirect_call': indirect.add(k); continue
        d = depth(t)
        if d[0] > best[0]: best = d
    onstack.discard(k)
    return (size.get(k, 0) + best[0], (k,) + best[1])
for root in sys.argv[2:]:
    d, path = depth(root)
    print(f'== {root}: {d} bytes')
    for k in path: print(f'  {size.get(k,0):5}  {label.get(k, k)[:100]}')
print('functions with indirect calls (not followed):', len(indirect))
print('recursion seen at:', ', '.join(label.get(c, c)[:50] for c in sorted(cycles)) or 'none')
if os.environ.get('CHILDREN'):
    rows = []
    def walk(k, base, lvl):
        for t in sorted(edges.get(k, ())):
            if t == '__indirect_call': continue
            rows.append((base + depth(t)[0], '  ' * lvl + label.get(t, t)[:60]))
            if lvl < int(os.environ['CHILDREN']) - 1: walk(t, base + size.get(t, 0), lvl + 1)
    walk(sys.argv[2], size.get(sys.argv[2], 0), 0)
    for d, n in sorted(rows, reverse=True)[:int(os.environ.get('TOP', 25))]: print(f'{d:6}  {n}')
