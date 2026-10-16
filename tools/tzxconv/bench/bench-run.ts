/*
This file is part of picoFace.

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with picoFace. If
not, see <https://www.gnu.org/licenses/>.

It runs in the CPU core of rp2040js (https://github.com/wokwi/rp2040js, MIT licence),
which is not part of this project and is not included.
*/

// Runs bench.bin on the Cortex-M0+ core of rp2040js with plain memory, counting cycles,
// and models the RP2040's XIP cache (16 KB, 2-way, 8-byte lines) for code and data in flash.
import { readFileSync } from 'node:fs';
import { CortexM0Core } from './src/cortex-m0-core.js';

const bin = readFileSync(process.argv[2]);
const missCost = Number(process.argv[3] ?? 50);         // cycles a cache miss costs
const flash = new Uint8Array(2 * 1024 * 1024); flash.set(bin);
const ram = new Uint8Array(256 * 1024);
const fv = new DataView(flash.buffer), rv = new DataView(ram.buffer);
// XIP cache
const SETS = 1024, tags = new Int32Array(SETS * 2).fill(-1), lru = new Uint8Array(SETS);
let misses = 0, accesses = 0;
function xip(addr: number) {
  accesses++;
  const line = (addr >>> 3), set = line & (SETS - 1), tag = line >>> 10;
  if (tags[set * 2] === tag) { lru[set] = 1; return; }
  if (tags[set * 2 + 1] === tag) { lru[set] = 0; return; }
  misses++;
  const way = lru[set] ? 1 : 0;           // replace the one not used last
  tags[set * 2 + way] = tag; lru[set] = way ? 0 : 1;
}
let breakCode = -1;
const chip: any = {
  logger: { debug() {}, info() {}, warn: console.warn, error: console.error },
  readUint32(a: number) { a >>>= 0; if (a < 0x20000000) { xip(a); return fv.getUint32(a - 0x10000000, true); } return rv.getUint32(a - 0x20000000, true); },
  readUint16(a: number) { a >>>= 0; if (a < 0x20000000) { xip(a); return fv.getUint16(a - 0x10000000, true); } return rv.getUint16(a - 0x20000000, true); },
  readUint8(a: number) { a >>>= 0; if (a < 0x20000000) { xip(a); return flash[a - 0x10000000]; } return ram[a - 0x20000000]; },
  writeUint32(a: number, v: number) { rv.setUint32((a >>> 0) - 0x20000000, v >>> 0, true); },
  writeUint16(a: number, v: number) { rv.setUint16((a >>> 0) - 0x20000000, v & 0xffff, true); },
  writeUint8(a: number, v: number) { ram[(a >>> 0) - 0x20000000] = v & 0xff; },
  onBreak(code: number) { breakCode = code; },
};
const core = new CortexM0Core(chip);
core.VTOR = 0x10000000;
core.reset();
const t0 = Date.now();
let frames = 0, last = 0;
const perFrame: number[] = [], missFrame: number[] = [];
let lastMiss = 0;
// PROF=1: cycles (and cache misses at missCost) by code address, shown by function (nm -S output in argv[5])
const prof = process.env.PROF ? new Float64Array(1 << 20) : null;
const stopAt = Number(process.env.MAXCYC ?? 0);
for (;;) {
  const pc0 = core.PC, c0 = core.cycles, m0 = misses;
  core.executeInstruction();
  if (prof) { prof[((pc0 - 0x10000000) >>> 1) & 0xFFFFF] += core.cycles - c0 + (misses - m0) * missCost; if (stopAt && core.cycles > stopAt) break; }
  if (breakCode >= 0) {
    const c = core.cycles;
    perFrame.push(c - last); missFrame.push(misses - lastMiss);
    last = c; lastMiss = misses;
    core.PC += 0;               // the BKPT has been stepped over
    if (breakCode === 2) break;
    breakCode = -1;
    frames++;
  }
}
const cycles = core.cycles;
const resAddr = 0;              // result[] is read by its symbol address, given as argv[4]
const res = Number(process.argv[4]) ? [0, 1, 2].map(i => rv.getUint32(Number(process.argv[4]) - 0x20000000 + 4 * i, true)) : [];
const T_FRAME = 69888, CLK = 125e6;
const total = cycles + misses * missCost;
console.log(`status ${res[0]} frames ${res[1]} game ok ${res[2]}`);
console.log(`cycles (all cache hits): ${cycles}  = ${(cycles / frames).toFixed(0)} a frame`);
console.log(`flash accesses ${accesses}, cache misses ${misses} (${(100 * misses / accesses).toFixed(3)}%)`);
console.log(`with ${missCost} cycles a miss: ${(total / frames).toFixed(0)} cycles a frame`);
const real = frames * 0.02, at125 = total / CLK;
console.log(`emulated ${real.toFixed(1)} s takes ${at125.toFixed(1)} s at 125 MHz: ${(real / at125).toFixed(2)}x real speed; at 250 MHz ${(2 * real / at125).toFixed(2)}x`);
// the frames while the tape is being loaded vs. the rest
const sorted = [...perFrame].sort((a, b) => a - b);
console.log(`cycles a frame: min ${sorted[0]}, median ${sorted[sorted.length >> 1]}, max ${sorted[sorted.length - 1]}`);
if (prof && process.argv[5]) {
  const syms = readFileSync(process.argv[5], 'utf8').split('\n').map(l => l.split(' ')).filter(p => p.length >= 4 && /[tTwW]/.test(p[2]))
    .map(p => ({ a: parseInt(p[0], 16), n: parseInt(p[1], 16), name: p.slice(3).join(' ') }));
  let all = 0; for (const v of prof) all += v;
  const rows = syms.map(s => { let v = 0; for (let a = s.a; a < s.a + s.n; a += 2) v += prof[((a - 0x10000000) >>> 1) & 0xFFFFF]; return { name: s.name, v, n: s.n }; })
    .filter(r => r.v > 0).sort((a, b) => b.v - a.v).slice(0, 30);
  for (const r of rows) console.log(`${(100 * r.v / all).toFixed(1).padStart(5)}%  ${String(r.n).padStart(5)} B  ${r.name.slice(0, 110)}`);
}
console.log(`(simulation took ${((Date.now() - t0) / 1000).toFixed(0)} s)`);
