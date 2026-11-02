#!/usr/bin/env python3
"""Proofs for framebuffer fetch (after DavidZande/MacNativeGames nvn_rt_fetch.h, here on the bridge's SPIR-V).

A fragment shader may read the target it renders into at its own pixel: it samples texture slot T at
in[L].xy / in[L].w, and the vertex shader writes in[L] as ((w + x) / 2, (w - y) / 2, -, w) of its final clip
position. With the sampled texture being that colour attachment (checked on the device per draw), the sample is the
fragment's own pixel and can read the attachment directly (input attachment, VK_KHR_dynamic_rendering_local_read):
no pass split, no copy. Both proofs are conservative: anything not recognised is refused and keeps the split path.

- prove_ps(words) -> [(sample result id, texture slot, varying location)]: implicit-LOD samples, or explicit with
  constant LOD 0, no offsets / Dref / gather, whose coordinate is (in[L].x / in[L].w, in[L].y / in[L].w) (also as
  x * (1 / w)), through the translator's NaN guard. A slot also sampled any other way is refused as a whole.
- prove_vs(words) -> {varying location}: the last store of each component of the output at that location and of
  gl_Position, as linear forms over the same values, satisfy out.x = (w + x) / 2, out.y = (w - y) / 2, out.w = w.

Values travel through the translator's Function-scope "registers": a register is tracked by its last store in
instruction order and dropped when any store to it lies in a block other than the entry block's straight line
(a block after the first OpBranchConditional / OpSwitch / OpLoopMerge), which would make the order meaningless.
"""
import struct
import sys
from fractions import Fraction

OP_NAME, OP_DECORATE, OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE = 5, 71, 32, 43, 59
OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 61, 62, 65, 66
OP_COMPOSITE_CONSTRUCT, OP_COMPOSITE_EXTRACT, OP_VECTOR_SHUFFLE = 80, 81, 79
OP_FNEGATE, OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV, OP_SELECT, OP_EXT_INST = 127, 129, 131, 133, 136, 169, 12
OP_SAMPLE_IMPLICIT, OP_SAMPLE_EXPLICIT = 87, 88
OP_LABEL, OP_BRANCH_COND, OP_SWITCH, OP_LOOP_MERGE, OP_SELECTION_MERGE, OP_FUNCTION_END = 248, 250, 251, 246, 247, 56
DEC_BUILTIN, DEC_LOCATION, BUILTIN_POSITION = 11, 30, 0
SC_INPUT, SC_OUTPUT, SC_FUNCTION = 1, 3, 7
GLSL_FMA = 50


def _insts(words):
    i = 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        if not n:
            raise ValueError('zero-length SPIR-V instruction')
        yield op, words[i:i + n]
        i += n


def _string(ws):
    return struct.pack('<%dI' % len(ws), *ws).split(b'\0', 1)[0].decode('utf-8', 'replace')


class _Module:
    def __init__(self, words):
        self.insts = list(_insts(words))
        self.names, self.deco, self.consts, self.storage, self.var_type, self.ptr = {}, {}, {}, {}, {}, {}
        self.float_types, self.const_type = set(), {}
        for op, a in self.insts:
            if op == 22 and len(a) >= 3 and a[2] == 32:   # OpTypeFloat 32
                self.float_types.add(a[1])
            if op == OP_NAME:
                self.names[a[1]] = _string(list(a[2:]))
            elif op == OP_DECORATE:
                self.deco.setdefault(a[1], {})[a[2]] = a[3] if len(a) > 3 else True
            elif op == OP_CONSTANT and len(a) == 4:
                self.consts[a[2]] = a[3]; self.const_type[a[2]] = a[1]
            elif op == OP_VARIABLE:
                self.storage[a[2]] = a[3]
                self.var_type[a[2]] = a[1]
            elif op == OP_TYPE_POINTER:
                self.ptr[a[1]] = a[3]
        # Registers stored in a non-straight-line part of the function are not tracked. The straight line is the entry
        # block and, after a structured selection made there (OpSelectionMerge, then a conditional branch or switch),
        # its merge block onwards: every path that has not ended (an alpha test's discard) runs that block after the
        # whole selection, so stores there still follow instruction order. A loop ends it for the rest of the function.
        # (Until 28 Oct 2026 the first branch ended it: an alpha test in front of an own-pixel sample hid the proof.)
        self.untracked, straight, resume, merge = set(), True, None, None
        self.cond = []   # per instruction: outside the straight line
        for op, a in self.insts:
            self.cond.append(not straight)
            if op == OP_FUNCTION_END:
                straight, resume, merge = True, None, None
            elif op == OP_LABEL and resume is not None and a[1] == resume:
                straight, resume = True, None
            elif not straight:
                if op == OP_STORE and self.storage.get(a[1]) == SC_FUNCTION:
                    self.untracked.add(a[1])
            elif op == OP_SELECTION_MERGE:
                merge = a[1]
            elif op in (OP_BRANCH_COND, OP_SWITCH):
                straight, resume, merge = False, merge, None   # no merge (a loop's branch): never resumes
            elif op == OP_LOOP_MERGE:
                straight, resume, merge = False, None, None

    def fconst(self, i):
        """A finite 32-bit float constant as an exact fraction, else None."""
        if i not in self.consts or self.const_type.get(i) not in self.float_types:
            return None
        f = struct.unpack('<f', struct.pack('<I', self.consts[i]))[0]
        return Fraction(f) if f == f and abs(f) != float('inf') else None


def _lin(e):
    """Linear form {atom: coefficient} of an expression, or None."""
    if e is None or e[0] == 'cond':
        return None
    k = e[0]
    if k == 'c':
        return {('one',): e[1]} if e[1] else {}
    if k in ('add', 'sub'):
        a, b = _lin(e[1]), _lin(e[2])
        if a is None or b is None:
            return None
        out = dict(a)
        for t, c in b.items():
            out[t] = out.get(t, 0) + (c if k == 'add' else -c)
        return {t: c for t, c in out.items() if c}
    if k == 'neg':
        a = _lin(e[1])
        return None if a is None else {t: -c for t, c in a.items()}
    if k == 'mul':
        for x, y in ((e[1], e[2]), (e[2], e[1])):
            if x and x[0] == 'c':
                b = _lin(y)
                return None if b is None else {t: c * x[1] for t, c in b.items() if c * x[1]}
        return {('atom', e): Fraction(1)}
    if k == 'fma':
        return _lin(('add', ('mul', e[1], e[2]), e[3]))
    return {('atom', e): Fraction(1)}


def _evaluate(m, want_samples):
    """Symbolic values of ids, plus samples (fragment) or last component stores per output (vertex)."""
    val, reg, samples, outs = {}, {}, [], {}
    for i, (op, a) in enumerate(m.insts):
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN):
            idx = tuple(m.consts.get(x, ('id', x)) for x in a[4:])
            val[a[2]] = ('ptr', a[3], idx)
        elif op == OP_LOAD:
            p = a[3]
            if p in reg:
                val[a[2]] = reg[p]
            elif p in val and val[p][0] == 'ptr' and m.storage.get(val[p][1]) == SC_INPUT:
                base, idx = val[p][1], val[p][2]
                loc = m.deco.get(base, {}).get(DEC_LOCATION)
                val[a[2]] = ('in', loc, idx[0]) if loc is not None and len(idx) == 1 and isinstance(idx[0], int) else ('ld', p)
            elif m.storage.get(p) == SC_INPUT and DEC_LOCATION in m.deco.get(p, {}):
                val[a[2]] = ('invec', m.deco[p][DEC_LOCATION])
            else:
                val[a[2]] = ('ld', val.get(p, ('v', p)))
        elif op == OP_STORE:
            target, v = a[1], val.get(a[2], ('v', a[2]))
            if m.storage.get(target) == SC_FUNCTION and target not in m.untracked:
                reg[target] = v
            if not want_samples:
                outs_store(m, val, outs, target, a[2], ('cond',) if m.cond[i] else v)   # a conditional store: unknown
        elif op == OP_COMPOSITE_EXTRACT and len(a) == 5:
            v = val.get(a[3])
            if v and v[0] == 'invec':
                val[a[2]] = ('in', v[1], a[4])
            elif v and v[0] == 'vec':
                val[a[2]] = v[1][a[4]] if a[4] < len(v[1]) else None
            else:
                val[a[2]] = ('x', v, a[4])
        elif op == OP_COMPOSITE_CONSTRUCT:
            val[a[2]] = ('vec', tuple(val.get(x) for x in a[3:]))
        elif op == OP_CONSTANT and m.fconst(a[2]) is not None:
            val[a[2]] = ('c', m.fconst(a[2]))
        elif op == OP_FNEGATE:
            val[a[2]] = ('neg', val.get(a[3]))
        elif op in (OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV):
            val[a[2]] = ({OP_FADD: 'add', OP_FSUB: 'sub', OP_FMUL: 'mul', OP_FDIV: 'div'}[op], val.get(a[3]), val.get(a[4]))
        elif op == OP_EXT_INST and len(a) == 8 and a[4] == GLSL_FMA:
            val[a[2]] = ('fma', val.get(a[5]), val.get(a[6]), val.get(a[7]))
        elif op == OP_SELECT:   # the translator's NaN guard: select(isnan(v), fallback, v)
            val[a[2]] = val.get(a[5])
        elif op == OP_VECTOR_SHUFFLE:
            v = val.get(a[3])
            val[a[2]] = v if v and a[3] == a[4] and list(a[5:]) == list(range(len(a) - 5)) else ('shuf', a[2])
        elif op in (OP_SAMPLE_IMPLICIT, OP_SAMPLE_EXPLICIT):
            samples.append((op, a, val.get(a[4]), val.get(a[3])))
        elif op == OP_LABEL:
            pass
    return val, samples, outs


def outs_store(m, val, outs, target, src, v):
    """Last store per (output location or 'pos', component)."""
    p = val.get(target)
    if p and p[0] == 'ptr':
        base, idx = p[1], p[2]
        comp = idx[-1] if idx and isinstance(idx[-1], int) else None
        key = _out_key(m, base, idx)
        if key is not None and comp is not None:
            outs[(key, comp)] = v
        return
    key = _out_key(m, target, ())
    if key is not None and v and v[0] == 'vec':
        for c, e in enumerate(v[1]):
            outs[(key, c)] = e
    elif key is not None:
        for c in range(4):
            outs[(key, c)] = v if v and v[0] == 'cond' else ('x', v, c)


def _out_key(m, var, idx):
    if m.storage.get(var) != SC_OUTPUT:
        return None
    d = m.deco.get(var, {})
    if d.get(DEC_BUILTIN) == BUILTIN_POSITION:
        return 'pos'
    if DEC_LOCATION in d:
        return d[DEC_LOCATION]
    if m.names.get(var, '').startswith('gl_') or idx:   # gl_PerVertex block: member 0 is the position
        return 'pos' if idx and idx[0] == 0 else None
    return None


def _texture_slot(m, image_val):
    """Texture slot of a sampled image loaded from a translator-named variable (shader_bridge.decode_tex)."""
    sys.path.insert(0, __import__('os').path.dirname(__file__))
    import shader_bridge
    if not image_val or image_val[0] != 'ld':
        return None
    inner = image_val[1]
    var = inner[1] if inner and inner[0] == 'v' else None
    if var is None:
        return None
    d = shader_bridge.decode_tex(m.names.get(var, ''))
    return d[0] if d and d[0] is not None else None


def _projective(coord):
    if not coord or coord[0] != 'vec' or len(coord[1]) != 2:
        return None
    def part(e):
        if e and e[0] == 'div' and e[1] and e[2] and e[1][0] == 'in' and e[2][0] == 'in' and e[1][1] == e[2][1]:
            return e[1][1], e[1][2], e[2][2]
        if e and e[0] == 'mul':
            for x, y in ((e[1], e[2]), (e[2], e[1])):
                if x and y and x[0] == 'in' and y[0] == 'div' and y[1] == ('c', 1) and y[2] and y[2][0] == 'in' and y[2][1] == x[1]:
                    return x[1], x[2], y[2][2]
        return None
    a, b = part(coord[1][0]), part(coord[1][1])
    if a and b and a[0] == b[0] and (a[1], b[1], a[2], b[2]) == (0, 1, 3, 3):
        return a[0]
    return None


def prove_ps(words):
    m = _Module(words)
    _, samples, _ = _evaluate(m, True)
    ok, bad = [], set()
    for op, a, coord, image in samples:
        slot = _texture_slot(m, image)
        if slot is None:
            continue
        operands = list(a[5:])
        plain = (op == OP_SAMPLE_IMPLICIT and (not operands or operands == [0])) or \
                (op == OP_SAMPLE_EXPLICIT and len(operands) == 2 and operands[0] == 2 and m.fconst(operands[1]) == 0)
        loc = _projective(coord) if plain else None
        if loc is None:
            bad.add(slot)
        else:
            ok.append((a[2], slot, loc))
    return [x for x in ok if x[1] not in bad]


OP_FUNCTION, OP_RETURN, OP_RETURN_VALUE, OP_KILL, OP_ENTRY_POINT = 54, 253, 254, 252, 15


def _exit_block(m):
    """(first, last) instruction indices of the entry function's only returning block, or None.

    Every invocation that finishes runs this block last and runs it straight through, so inside it instruction order
    is execution order, whatever branches came before it."""
    entry = next((a[2] for op, a in m.insts if op == OP_ENTRY_POINT), None)
    func, start, exits = None, None, []
    for i, (op, a) in enumerate(m.insts):
        if op == OP_FUNCTION:
            func = a[2]
        elif op == OP_FUNCTION_END:
            func = None
        elif func == entry and op == OP_LABEL:
            start = i
        elif func == entry and op in (OP_RETURN, OP_RETURN_VALUE, OP_KILL):
            exits.append((start, i))
    return exits[0] if len(exits) == 1 and exits[0][0] is not None else None


def _evaluate_exit(m, block):
    """Last component stores per output made inside the exit block. Registers inside it follow their stores in
    order; a register read before any store in the block is the value it had on entry (one symbol per register,
    valid because nothing stores to it in between). A register read outside the block is a fresh symbol per load."""
    lo, hi = block
    val, reg, outs = {}, {}, {}
    for i, (op, a) in enumerate(m.insts):
        inside = lo <= i <= hi
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN):
            idx = tuple(m.consts.get(x, ('id', x)) for x in a[4:])
            val[a[2]] = ('ptr', a[3], idx)
        elif op == OP_LOAD:
            p = a[3]
            if m.storage.get(p) == SC_FUNCTION:
                val[a[2]] = (reg[p] if p in reg else ('entry', p)) if inside else ('ssa', a[2])
            elif p in val and val[p][0] == 'ptr' and m.storage.get(val[p][1]) == SC_INPUT:
                base, idx = val[p][1], val[p][2]
                loc = m.deco.get(base, {}).get(DEC_LOCATION)
                val[a[2]] = ('in', loc, idx[0]) if loc is not None and len(idx) == 1 and isinstance(idx[0], int) else ('ssa', a[2])
            else:
                val[a[2]] = ('ssa', a[2])
        elif op == OP_STORE:
            if not inside:
                continue
            target, v = a[1], val.get(a[2], ('ssa', a[2]))
            if m.storage.get(target) == SC_FUNCTION:
                reg[target] = v
            else:
                outs_store(m, val, outs, target, a[2], v)
        elif op == OP_COMPOSITE_EXTRACT and len(a) == 5:
            v = val.get(a[3])
            val[a[2]] = v[1][a[4]] if v and v[0] == 'vec' and a[4] < len(v[1]) else ('x', v, a[4])
        elif op == OP_COMPOSITE_CONSTRUCT:
            val[a[2]] = ('vec', tuple(val.get(x) for x in a[3:]))
        elif op == OP_CONSTANT and m.fconst(a[2]) is not None:
            val[a[2]] = ('c', m.fconst(a[2]))
        elif op == OP_FNEGATE:
            val[a[2]] = ('neg', val.get(a[3]))
        elif op in (OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV):
            val[a[2]] = ({OP_FADD: 'add', OP_FSUB: 'sub', OP_FMUL: 'mul', OP_FDIV: 'div'}[op], val.get(a[3]), val.get(a[4]))
        elif op == OP_EXT_INST and len(a) == 8 and a[4] == GLSL_FMA:
            val[a[2]] = ('fma', val.get(a[5]), val.get(a[6]), val.get(a[7]))
        elif op == OP_SELECT:   # the translator's NaN guard: select(isnan(v), fallback, v)
            val[a[2]] = val.get(a[5])
    return outs


def _vs_locations(outs):
    pos = [_lin(outs.get(('pos', c))) for c in range(4)]
    if any(p is None for p in (pos[0], pos[1], pos[3])):
        return set()
    def comb(*terms):
        out = {}
        for coeff, form in terms:
            for t, c in form.items():
                out[t] = out.get(t, 0) + coeff * c
        return {t: c for t, c in out.items() if c}
    half = Fraction(1, 2)
    want_x, want_y = comb((half, pos[3]), (half, pos[0])), comb((half, pos[3]), (-half, pos[1]))
    proven = set()
    for loc in {k for k, _ in outs if isinstance(k, int)}:
        x, y, w = (_lin(outs.get((loc, c))) for c in (0, 1, 3))
        if x == want_x and y == want_y and w == comb((Fraction(1), pos[3])) and x:
            proven.add(loc)
    return proven


def prove_vs(words):
    """Locations L where the vertex shader writes ((w + x) / 2, (w - y) / 2, -, w) of its final clip position: over
    the straight-line part of the function, or (when branches come earlier) within its only returning block."""
    m = _Module(words)
    _, _, outs = _evaluate(m, False)
    proven = _vs_locations(outs)
    block = _exit_block(m)
    if block:
        proven |= _vs_locations(_evaluate_exit(m, block))
    return proven


def reply_words(data):
    head = data[:data.index(b'\n')].split()
    n = int(head[1])
    return struct.unpack('<%dI' % (n // 4), data[-n:])


if __name__ == '__main__':
    for path in sys.argv[1:]:
        w = reply_words(open(path, 'rb').read())
        print(path, 'ps', prove_ps(w), 'vs', sorted(prove_vs(w)))
