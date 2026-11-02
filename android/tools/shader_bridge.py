#!/usr/bin/env python3
"""Shader translation bridge for the Android game loader (development).

The Thor sends one request per line over TCP (adb reverse tcp:27760):
  {"variant", "stage": "vs|ps|cs", "code_hex", "c1", "tex", "img", "attr",
   "fout", "local": [x, y, z] (compute), "depth_minus_w": bool}
The bridge runs the macOS port's prebuilt translator (totk_xlate --serve, the
vendored Ryujinx shader translator) and replies with Vulkan SPIR-V plus a
binding table derived from the SPIR-V resource names by the same rules as
games/totk/tools/totk_xlate/Service.cs (Reflect / DecodeTex):

  OK <spv_bytes> <nbind> <ninput> <noutput> <lx> <ly> <lz>
  B <set> <binding> <kind> <reg> <sreg> <off> <rw> <descriptor_type> <count> <view>
  I <location> <float|sint|uint>      (stage inputs: vertex attributes / fragment varyings)
  O <location> <float|sint|uint>      (stage outputs: vertex varyings / fragment colours)
  <spv bytes>
or
  ERR <message>

kind uses nvn_refl.h numbering (CBUF 1, TEX 3, IMG 4, TBUF 5, SBUFP 6,
SUPPORT 7, C1 8, DRV0 9, DRV2 10, UBOP 12, SBUFU 13). For a combined image
sampler, sreg is the separate sampler slot, or -1 when the sampler comes from
the combined texture handle. view is the VkImageViewType the shader declares
(+256 when multisampled, -1 for non-image resources). Descriptor sets are rewritten so the vertex stage
uses set 0, the fragment stage set 1 and compute set 0.

A request {"variant": V, "strip_inputs": MASK} returns variant V (already
translated) with the fragment inputs at the MASK locations replaced by their
Maxwell default (0, 0, 0, 1 for float vec4, else zero): the Adreno driver
refuses fragment inputs the vertex stage does not write, which NVN allows.

Vertex shaders start by storing (0, 0, 0, 1) to every float vec4 output: on
Maxwell the components a vertex shader never writes read as (0, 0, 0, 1), in
Vulkan they are undefined (zero on Adreno). TOTK multiplies such a w into
alphas and matrices (MacNativeGames PR #91, translator patch 0005).

usage: shader_bridge.py [--port 27760] [--work DIR]
"""
import argparse
import os
import re
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
XLATE = os.path.join(REPO, 'games', 'totk', 'prebuilt', 'macos-arm64', 'totk_xlate')
# Bumped whenever the reply format or the SPIR-V rewriting changes: cached replies of older versions are not reused.
# 3: vertex outputs start at (0, 0, 0, 1); version-2 replies are upgraded without translating again.
# 4: 2D gathers nudged by 1/512 texel (Spirv.nudge_gathers); version-3 replies are upgraded without translating again.
REPLY_VERSION = 4

TEX_BASE, SAMP_BASE, IMG_BASE = 0x168, 0x568, 0x120
COMBINED_BASE, COMBINED_SLOTS = 0x20, 32
K_CBUF, K_TEX, K_IMG, K_TBUF, K_SBUFP, K_SUPPORT, K_C1, K_DRV0, K_DRV2, K_UBOP, K_SBUFU = 1, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13
VK_SAMPLER, VK_COMBINED, VK_SAMPLED, VK_STORAGE_IMAGE, VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL, VK_UBO, VK_SSBO = 0, 1, 2, 3, 4, 5, 6, 7


def slot(word, base, cb):
    if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0:
        return (word * 4 - base) // 8
    return None


def decode_tex(name):
    """Service.cs DecodeTex: (texture slot, sampler slot, handle type) from a resource name."""
    m = re.match(r'^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m:
        return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2
    sb = (slots >> 16) - 1 if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    words = [o0, o1] if ty else [o0]
    if re.match(r'^[a-z]+_i', name):
        for x in words:
            if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None:
                return slot(x, IMG_BASE, tb), None, ty
        return None, None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    if t is None:
        t = next((slot(x, COMBINED_BASE, tb) for x in words
                  if COMBINED_BASE <= x * 4 < COMBINED_BASE + 8 * COMBINED_SLOTS and slot(x, COMBINED_BASE, tb) is not None), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty


class Spirv:
    """Just enough SPIR-V parsing for resource and interface reflection."""

    def __init__(self, data):
        if len(data) < 20 or len(data) % 4:
            raise ValueError('not SPIR-V')
        self.words = list(struct.unpack('<%dI' % (len(data) // 4), data))
        if self.words[0] != 0x07230203:
            raise ValueError('bad SPIR-V magic')
        self.names, self.deco, self.types, self.vars, self.consts, self.caps = {}, {}, {}, {}, {}, set()
        self.deco_at = []   # (word index of the operand, id, decoration) for rewriting sets / bindings
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            if n == 0:
                raise ValueError('zero-length SPIR-V instruction')
            if op == 17:
                self.caps.add(w[1])
            elif op == 5:
                self.names[w[1]] = self._string(w[2:])
            elif op == 71:
                self.deco.setdefault(w[1], {})[w[2]] = w[3] if n > 3 else True
                if w[2] in (33, 34):
                    self.deco_at.append((i + 3, w[1], w[2]))
            elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32):
                self.types[w[1]] = (op, w[2:])
            elif op == 43:
                self.consts[w[2]] = w[3]
            elif op == 59:
                self.vars[w[2]] = (w[1], w[3])
            i += n

    @staticmethod
    def _string(ws):
        b = struct.pack('<%dI' % len(ws), *ws)
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def pointee(self, var):
        ptr_type, storage = self.vars[var]
        op, ops = self.types[ptr_type]
        return storage, ops[1]

    def view_type(self, image_ops):
        dim, arrayed, ms = image_ops[1], image_ops[3], image_ops[4]
        base = {0: (0, 4), 1: (1, 5), 2: (2, 2), 3: (3, 6), 4: (1, 5), 6: (1, 5)}.get(dim)
        if base is None:
            return -1
        return base[1 if arrayed else 0] + (256 if ms else 0)

    def descriptor(self, var):
        """(descriptor type, count, view type) of a resource variable."""
        storage, t = self.pointee(var)
        count = 1
        while self.types[t][0] in (28, 29):   # (runtime) arrays of descriptors
            op, ops = self.types[t]
            count *= self.consts.get(ops[1], 1) if op == 28 else 1
            t = ops[0]
        op, ops = self.types[t]
        if op == 27:
            image = self.types[ops[0]][1]
            return (VK_UNIFORM_TEXEL if image[1] == 5 else VK_COMBINED), count, self.view_type(image)
        if op == 26:
            return VK_SAMPLER, count, -1
        if op == 25:
            dim, sampled = ops[1], ops[5]
            if dim == 5:
                return (VK_STORAGE_TEXEL if sampled == 2 else VK_UNIFORM_TEXEL), count, -1
            return (VK_STORAGE_IMAGE if sampled == 2 else VK_SAMPLED), count, self.view_type(ops)
        if op == 30:
            if storage == 12 or 3 in self.deco.get(t, {}):
                return VK_SSBO, count, -1
            return VK_UBO, count, -1
        raise ValueError('unsupported resource type %d for %s' % (op, self.names.get(var, var)))

    def scalar_class(self, t):
        while self.types[t][0] in (23, 28):   # vectors, arrays
            t = self.types[t][1][0]
        op, ops = self.types[t]
        if op == 21:
            return 'sint' if ops[1] else 'uint'
        return 'float'

    def interface(self, storage_class):
        out = []
        for var, (ptr, storage) in self.vars.items():
            d = self.deco.get(var, {})
            if storage != storage_class or 30 not in d or 11 in d:   # Location set, not a BuiltIn
                continue
            out.append((d[30], self.scalar_class(self.types[ptr][1][1])))
        return sorted(out)

    def strip_interlock(self):
        """Removes SPV_EXT_fragment_shader_interlock (absent from the Adreno driver):
        the critical section runs without per-pixel ordering. Returns True if removed."""
        out, i, removed = self.words[:5], 5, False
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            drop = (op == 17 and w[1] in (5363, 5372, 5378)) or op in (5364, 5365) \
                or (op == 16 and w[2] in (5366, 5367, 5368, 5369, 5370, 5371)) \
                or (op == 10 and self._string(w[1:]) == 'SPV_EXT_fragment_shader_interlock')
            if drop:
                removed = True
            else:
                out += w
            i += n
        self.words = out
        self.deco_at = []
        return removed

    def zero_inputs(self, mask):
        """Turns Input variables at the given locations into zero-initialized
        Private variables (SPIR-V 1.3: removed from the entry point interface).
        Access chains into them get Private pointer types."""
        targets = {v for v, (ptr, st) in self.vars.items() if st == 1 and self.deco.get(v, {}).get(30, -1) >= 0
                   and 11 not in self.deco.get(v, {}) and (mask >> self.deco[v][30]) & 1}
        if not targets:
            return
        # pass 1: ids derived from the targets and the pointee types needing Private pointers
        derived, pointees = set(targets), {self.types[self.vars[v][0]][1][1] for v in targets}
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            if op in (65, 66) and self.words[i + 3] in derived:
                derived.add(self.words[i + 2])
                pointees.add(self.types[self.words[i + 1]][1][1])
            i += n
        bound = self.words[3]
        new_ptr = {}
        for t in sorted(pointees):
            new_ptr[t] = bound
            bound += 1
        nulls, decls = {}, []
        for t, pid in new_ptr.items():
            decls += [(4 << 16) | 32, pid, 6, t]
        for v in sorted(targets):   # id order: the device's C port (nvn_vk_shader.c) emits the same words
            t = self.types[self.vars[v][0]][1][1]
            if t in nulls:
                continue
            if self.vec4_float(t):   # Maxwell default of an unwritten attribute
                d, nulls[t], bound = self.default_vec4(t, bound)
                decls += d
            else:
                nulls[t] = bound
                decls += [(3 << 16) | 46, t, bound]
                bound += 1
        # pass 2: rewrite
        out, i, inserted = self.words[:5], 5, False
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = list(self.words[i:i + n])
            if op == 15:   # OpEntryPoint: drop the targets from the interface
                k = 3
                while True:   # the name is a nul-terminated string
                    word = w[k]
                    k += 1
                    if any(((word >> sh) & 0xff) == 0 for sh in (0, 8, 16, 24)):
                        break
                w = w[:k] + [x for x in w[k:] if x not in targets]
                w[0] = (len(w) << 16) | op
            elif op == 71 and w[1] in targets:
                i += n
                continue
            elif op == 59 and w[2] in targets:
                if not inserted:
                    out += decls
                    inserted = True
                t = self.types[w[1]][1][1]
                w = [(5 << 16) | 59, new_ptr[t], w[2], 6, nulls[t]]
            elif op in (65, 66) and w[3] in derived:
                w[1] = new_ptr[self.types[w[1]][1][1]]
            out += w
            i += n
        out[3] = bound
        self.words = out
        self.deco_at = []

    def vec4_float(self, t):
        """True for a 32-bit float vec4 type id."""
        op, ops = self.types.get(t, (0, []))
        if op != 23 or ops[1] != 4:
            return False
        e = self.types.get(ops[0], (0, []))
        return e[0] == 22 and e[1][0] == 32

    def default_vec4(self, t, bound):
        """Declarations of (0, 0, 0, 1) of vec4 type t: (words, id, new bound)."""
        f = self.types[t][1][0]
        zero, one, vec = bound, bound + 1, bound + 2
        return [(4 << 16) | 43, f, zero, 0, (4 << 16) | 43, f, one, 0x3f800000,
                (7 << 16) | 44, t, vec, zero, zero, zero, one], vec, bound + 3

    def init_outputs(self):
        """Vertex stage: stores (0, 0, 0, 1) to every float vec4 user output at the start of the entry point.
        The shader's own stores follow, so only components it never writes keep the default. Returns the count."""
        outs = [v for v, (ptr, st) in self.vars.items() if st == 3 and 30 in self.deco.get(v, {})
                and 11 not in self.deco.get(v, {}) and self.vec4_float(self.types[ptr][1][1])]
        entry = None
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            if op == 15:
                entry = self.words[i + 2]
                break
            i += n
        if not outs or entry is None:
            return 0
        decls, consts, bound = [], {}, self.words[3]
        for v in outs:
            t = self.types[self.vars[v][0]][1][1]
            if t not in consts:
                d, consts[t], bound = self.default_vec4(t, bound)
                decls += d
        out, i, state = self.words[:5], 5, 0   # 0: globals, 1: in the entry function, 2: done
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            if op == 54 and decls:   # first function: the constants go just before it
                out += decls
                decls = []
            if state == 0 and op == 54 and w[2] == entry:
                state = 1
            elif state == 1 and op == 248:   # first block: after its OpVariables
                out += w
                i += n
                while (self.words[i] & 0xffff) == 59:
                    k = self.words[i] >> 16
                    out += self.words[i:i + k]
                    i += k
                for v in outs:
                    out += [(3 << 16) | 62, v, consts[self.types[self.vars[v][0]][1][1]]]
                state = 2
                continue
            out += w
            i += n
        if state != 2:
            raise ValueError('vertex output initialisation found no entry block')
        out[3] = bound
        self.words = out
        self.deco_at = []
        return len(outs)

    def nudge_gathers(self):
        """Adds 1/512 texel to the x and y coordinate of every 2D / 2D array OpImageGather (Ryujinx
        InsertCoordGatherBias with precision 8, yuzu need_gather_subpixel_offset). A coordinate exactly between two
        texels gathers the next texel on Adreno and the previous one on Maxwell; TOTK's sun shadow pass turns that
        texel into 64x64 dark squares on lit ground (Lemon 1.2.2, 22 Oct 2026). Returns (nudged, skipped)."""
        def find(op, ops):
            return next((t for t, (o, s) in self.types.items() if o == op and s == ops), None)
        f32 = find(22, [32])
        if f32 is None:
            return 0, 0
        result_type, gathers, skipped = {}, [], 0
        fadd, fdiv, nudges = {}, {}, set()   # to recognise a gather nudged already
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            if op in (61, 86, 83, 245, 169) and n > 2:   # load, sampled image, copy, phi, select
                result_type[self.words[i + 2]] = self.words[i + 1]
            elif op == 43 and n == 4 and self.words[i + 3] == 0x3b000000:
                nudges.add(self.words[i + 2])
            elif op == 44 and n >= 5 and self.words[i + 3] in nudges and self.words[i + 4] in nudges:
                nudges.add(self.words[i + 2])
            elif op == 129:
                fadd[self.words[i + 2]] = self.words[i + 4]
            elif op == 136:
                fdiv[self.words[i + 2]] = self.words[i + 3]
            elif op == 96:
                gathers.append(i)
            elif op in (97, 314, 315):   # dref and sparse gathers: none in TOTK's seed, left as they are
                skipped += 1
            i += n
        plan = {}
        for at in gathers:
            if fdiv.get(fadd.get(self.words[at + 4])) in nudges:
                continue
            t = self.types.get(result_type.get(self.words[at + 3]), (0, []))
            image = self.types.get(t[1][0], (0, [])) if t[0] == 27 else (0, [])
            # image ops: sampled type, dim, depth, arrayed, ms, sampled
            if image[0] != 25 or image[1][1] != 1 or image[1][4]:
                skipped += 1
                continue
            plan[at] = (t[1][0], 3 if image[1][3] else 2)
        if not plan:
            return 0, skipped
        bound, decls = self.words[3], []
        def declare(op, ops):
            nonlocal bound
            t = find(op, ops)
            if t is None:
                t, bound = bound, bound + 1
                decls.extend([((len(ops) + 2) << 16) | op, t] + ops)
                self.types[t] = (op, ops)
            return t
        i32 = declare(21, [32, 1])
        zero_i, zero_f, nudge = bound, bound + 1, bound + 2
        bound += 3
        decls += [(4 << 16) | 43, i32, zero_i, 0, (4 << 16) | 43, f32, zero_f, 0, (4 << 16) | 43, f32, nudge, 0x3b000000]
        vec = {}
        for dims in sorted({d for _, d in plan.values()}):
            fv, iv = declare(23, [f32, dims]), declare(23, [i32, dims])
            c, bound = bound, bound + 1
            parts = [nudge, nudge] + [zero_f] * (dims - 2)   # the array layer stays
            decls += [((3 + dims) << 16) | 44, fv, c] + parts
            vec[dims] = (fv, iv, c)
        out, i, declared, caps_done = self.words[:5], 5, False, 50 in self.caps
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = list(self.words[i:i + n])
            if not caps_done and op != 17:   # after the last capability: ImageQuery for OpImageQuerySizeLod
                out += [(2 << 16) | 17, 50]
                caps_done = True
            if op == 54 and not declared:
                out += decls
                declared = True
            if i in plan:
                image_type, dims = plan[i]
                fv, iv, c = vec[dims]
                img, size, sizef, off, coord = range(bound, bound + 5)
                bound += 5
                out += [(4 << 16) | 100, image_type, img, w[3],
                        (5 << 16) | 103, iv, size, img, zero_i,
                        (4 << 16) | 111, fv, sizef, size,
                        (5 << 16) | 136, fv, off, c, sizef,
                        (5 << 16) | 129, fv, coord, w[4], off]
                w[4] = coord
            out += w
            i += n
        out[3] = bound
        self.words = out
        self.deco_at = []
        return len(plan), skipped

    def rewrite_set(self, new_set):
        for at, var, kind in self.deco_at:
            if kind == 34:
                self.words[at] = new_set
        return struct.pack('<%dI' % len(self.words), *self.words)


def classify(stage, spv, res):
    """Service.cs Reflect on SPIR-V names; returns binding rows."""
    rows = []
    sbufs = sorted(res.get('sbufs', []), key=lambda b: b['binding'])
    base = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}[stage]
    sb_index = 0
    resources = sorted((spv.deco[v].get(33, 0), v) for v, (p, st) in spv.vars.items()
                       if st in (0, 2, 12) and 33 in spv.deco.get(v, {}))
    for binding, var in resources:
        name = spv.names.get(var, '')
        dtype, count, view = spv.descriptor(var)
        row = dict(binding=binding, kind=0, reg=-1, sreg=-1, off=0, rw=0, dtype=dtype, count=count, view=view)
        m = re.match(r'^[a-z]+_c(\d+)$', name)
        if name == 'support_buffer':
            row['kind'] = K_SUPPORT
        elif m:
            n = int(m.group(1))
            if 3 <= n <= 16:
                row.update(kind=K_CBUF, reg=n - 3)
            elif n == 1:
                row['kind'] = K_C1
            elif n in (0, 2):
                row['kind'] = K_DRV0 if n == 0 else K_DRV2
            else:
                raise ValueError('constant bank %d in %s' % (n, name))
        elif re.match(r'^[a-z]+_s(\d+)$', name):
            b = sbufs[sb_index] if sb_index < len(sbufs) else None
            sb_index += 1
            cb_slot = b['cb_slot'] if b else 0
            off = b['cb_offset'] * 4 if b and cb_slot == 0 else None
            row['rw'] = 1 if b and (b['flags'] & 1) else 0   # Ryujinx BufferUsageFlags.Write
            if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0:
                row.update(kind=K_UBOP, reg=(off - 0x210) // 16)
            elif b and 3 <= cb_slot <= 16:
                row.update(kind=K_SBUFU, reg=cb_slot - 3, off=b['cb_offset'] * 4)
            elif off is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0:
                row.update(kind=K_SBUFP, reg=(off - base) // 16)
            else:
                raise ValueError('storage buffer %s: address source c%d+%#x not mapped' % (name, cb_slot, (b or {}).get('cb_offset', 0) * 4))
        else:
            d = decode_tex(name)
            if d is None or d[0] is None:
                raise ValueError('resource %s has no NVN slot' % name)
            t, s, ty = d
            if re.match(r'^[a-z]+_i', name):
                row.update(kind=K_IMG, reg=t)
            elif dtype in (VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL):
                row.update(kind=K_TBUF, reg=t)
            else:
                row.update(kind=K_TEX, reg=t, sreg=s if ty else -1)
        rows.append(row)
    return rows


class Translator:
    def __init__(self, work):
        self.work = work
        self.lock = threading.Lock()
        self.proc = subprocess.Popen([XLATE, '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def run(self, job):
        import json
        with self.lock:
            self.proc.stdin.write(json.dumps({'job': job, 'out': self.work}) + '\n')
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('translator exited')
        if not line.startswith('ok '):
            raise RuntimeError(line.strip()[:300])
        with open(os.path.join(self.work, job['key'] + '.spv'), 'rb') as f:
            spv = f.read()
        with open(os.path.join(self.work, job['key'] + '.res.json')) as f:
            res = json.load(f)
        return spv, res


def split_reply(reply):
    """(header and table lines, SPIR-V bytes) of a reply."""
    lines, pos = [], 0
    head = reply[:reply.index(b'\n')].decode().split()
    total = int(head[2]) + int(head[3]) + int(head[4])
    for _ in range(total + 1):
        end = reply.index(b'\n', pos)
        lines.append(reply[pos:end].decode())
        pos = end + 1
    return lines, reply[pos:]


def validate(work, name, data):
    check = os.path.join(work, name + '.spv')
    with open(check, 'wb') as f:
        f.write(data)
    val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
    if val.returncode:
        raise ValueError('rewritten SPIR-V is invalid: ' + (val.stderr or val.stdout).strip()[:200])


def vertex_defaults(work, name, data):
    """Vertex SPIR-V with outputs initialised to (0, 0, 0, 1) (validated when anything changed)."""
    spv = Spirv(data)
    if not spv.init_outputs():
        return data
    data = struct.pack('<%dI' % len(spv.words), *spv.words)
    validate(work, name + '-defaults', data)
    return data


def gather_nudge(work, name, data):
    """SPIR-V with its 2D gathers nudged off the texel boundary (validated when anything changed)."""
    spv = Spirv(data)
    if not spv.nudge_gathers()[0]:
        return data
    nudged = struct.pack('<%dI' % len(spv.words), *spv.words)
    try:
        validate(work, name + '-gather', nudged)
    except ValueError:
        try:
            validate(work, name + '-plain', data)
        except ValueError:   # invalid before the nudge already (4 of TOTK's seed, accepted by the driver): left as is
            print('[bridge] %s: SPIR-V invalid before the gather nudge, not nudged' % name, flush=True)
            return data
        raise
    return nudged


def upgrade_reply(tr, variant, stage, old, version=2):
    """An older reply in the current format: vertex outputs get their defaults (version 2), gathers their
    nudge (versions 2 and 3)."""
    lines, data = split_reply(old)
    if version < 3 and stage == 'vs':
        data = vertex_defaults(tr.work, variant, data)
    data = gather_nudge(tr.work, variant, data)
    head = lines[0].split()
    head[1] = str(len(data))
    return ('\n'.join([' '.join(head)] + lines[1:]) + '\n').encode() + data


def strip_inputs(tr, rq):
    variant, mask = rq['variant'], int(rq['strip_inputs'])
    out = os.path.join(tr.work, '%s-i%x.reply%d' % (variant, mask, REPLY_VERSION))
    if os.path.exists(out):
        with open(out, 'rb') as f:
            return f.read()
    with open(os.path.join(tr.work, '%s.reply%d' % (variant, REPLY_VERSION)), 'rb') as f:
        reply = f.read()
    lines, data = split_reply(reply)
    head = lines[0].split()
    spv = Spirv(data)
    spv.zero_inputs(mask)
    data = struct.pack('<%dI' % len(spv.words), *spv.words)
    check = os.path.join(tr.work, '%s-i%x.spv' % (variant, mask))
    with open(check, 'wb') as f:
        f.write(data)
    val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
    if val.returncode:
        raise ValueError('input stripping produced invalid SPIR-V: ' + (val.stderr or val.stdout).strip()[:200])
    kept = [l for l in lines[1:] if not (l.startswith('I ') and (mask >> int(l.split()[1])) & 1)]
    head[1] = str(len(data))
    head[3] = str(sum(1 for l in kept if l.startswith('I ')))
    result = ('\n'.join([' '.join(head)] + kept) + '\n').encode() + data
    with open(out, 'wb') as f:
        f.write(result)
    return result


def handle(tr, rq):
    import json
    if 'strip_inputs' in rq:
        return strip_inputs(tr, rq)
    variant, stage = rq['variant'], rq['stage']
    if stage not in ('vs', 'ps', 'cs'):
        raise ValueError('stage %s has no translation chain' % stage)
    done = os.path.join(tr.work, '%s.reply%d' % (variant, REPLY_VERSION))
    if os.path.exists(done):   # the variant name covers the code and every answer
        with open(done, 'rb') as f:
            return f.read()
    for version in (3, 2):
        old = os.path.join(tr.work, '%s.reply%d' % (variant, version))
        if os.path.exists(old):
            with open(old, 'rb') as f:
                reply = upgrade_reply(tr, variant, stage, f.read(), version)
            with open(done + '.tmp', 'wb') as f:
                f.write(reply)
            os.replace(done + '.tmp', done)
            return reply
    code = bytes.fromhex(rq['code_hex'])
    path = os.path.join(tr.work, variant + '.code')
    with open(path, 'wb') as f:
        f.write(code)
    job = {'key': variant, 'stage': stage, 'code': path, 'c1': rq.get('c1', ''), 'debug': True, 'reduced': True,
           'tex': rq.get('tex', {}), 'img': rq.get('img', {}), 'attr': rq.get('attr', {}), 'fout': rq.get('fout', {}),
           'depth_minus_w': bool(rq.get('depth_minus_w'))}
    if rq.get('local'):
        job['local'] = rq['local']
    data, res = tr.run(job)
    spv = Spirv(data)
    if 10 in spv.caps:   # Float64: the Adreno driver has no shaderFloat64; use the translator's lowering
        job['softfloat64'] = True
        data, res = tr.run(job)
        spv = Spirv(data)
        if 10 in spv.caps:
            raise ValueError('FP64 lowering still emitted Float64')
    if 11 in spv.caps:
        raise ValueError('shader needs Int64, which the Adreno driver lacks')
    rows = classify(stage, spv, res)
    if spv.strip_interlock():
        print('[bridge] %s: fragment shader interlock removed (no ordering guarantee on this device)' % variant, flush=True)
        spv = Spirv(struct.pack('<%dI' % len(spv.words), *spv.words))
    data = spv.rewrite_set(1 if stage == 'ps' else 0)
    if stage == 'vs':
        data = vertex_defaults(tr.work, variant, data)
    data = gather_nudge(tr.work, variant, data)
    inputs = spv.interface(1) if stage in ('vs', 'ps') else []
    outputs = spv.interface(3) if stage in ('vs', 'ps') else []
    local = rq.get('local') or [0, 0, 0]
    head = ['OK %d %d %d %d %d %d %d' % (len(data), len(rows), len(inputs), len(outputs), local[0], local[1], local[2])]
    for r in rows:
        head.append('B %d %d %d %d %d %d %d %d %d %d' % (1 if stage == 'ps' else 0, r['binding'], r['kind'], r['reg'], r['sreg'], r['off'], r['rw'], r['dtype'], r['count'], r['view']))
    head += ['I %d %s' % x for x in inputs] + ['O %d %s' % x for x in outputs]
    reply = ('\n'.join(head) + '\n').encode() + data
    with open(done + '.tmp', 'wb') as f:
        f.write(reply)
    os.replace(done + '.tmp', done)
    return reply


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        import json
        for line in self.rfile:
            if not line.strip():
                continue
            try:
                rq = json.loads(line)
                reply = handle(self.server.translator, rq)
                print('[bridge] %s %s ok (%d bytes)' % (rq.get('stage'), rq.get('variant'), len(reply)), flush=True)
            except Exception as e:   # reported to the device, which stops with the message
                msg = ('%s: %s' % (type(e).__name__, e)).replace('\n', ' ')[:400]
                print('[bridge] FAIL %s' % msg, flush=True)
                reply = ('ERR %s\n' % msg).encode()
            self.wfile.write(reply)
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=27760)
    ap.add_argument('--work', default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix='totk-shader-bridge-')
    os.makedirs(work, exist_ok=True)
    if not os.access(XLATE, os.X_OK):
        sys.exit('translator missing: %s' % XLATE)
    srv = Server(('127.0.0.1', a.port), Handler)
    srv.translator = Translator(work)
    print('[bridge] listening on 127.0.0.1:%d, work %s' % (a.port, work), flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
