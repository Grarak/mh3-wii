#!/usr/bin/env python3
"""Find the guest reads of MH3's step counters (system_w+0xC, incremented once
per logic step by RunTasks 0x800416F8, and the scene counters in COUNTERS)
that test them periodically:
`counter % N == k` ("every N steps, at phase k"). Writes
runtime/src/step_counter_sites.inc: one {pc, N, k, kind} per site, where kind
is 1 for an equality test (an event on the steps the phase comes round), 2 for
one on a player's step counter (MANUAL) and 0 for any other use of the phase
(read as the tick count). The translator hooks
the listed loads (translation.load_hooks in recomp.yml) and the runtime
(step_counter.cpp) answers them in ticks of game time.

usage: step_counter_sites.py main.s [overlay.s ...]
  main.s: llvm-objdump -d of build/ghidra/main.elf
  overlay.s: an RSO overlay's code, from its linked analysis DOL
    (scripts/dol2elf.py --dol build/rso-overlays/P/analysis.dol --out P.elf
     --symbols build/rso-overlays/P/MAP.txt symbols.txt;
     llvm-objdump -d --no-show-raw-insn --start-address=0x80800000 P.elf).
    The overlays are translated with the same hooks (translate_rso_overlays.py),
    so their sites go in the same table; only their code at 0x80800000 and up
    is scanned (below it is the DOL's). The map overlays share a load address:
    a site at a pc two overlays share is an error.
"""
import re
import sys

# Counters the runtime advances by the ticks of each step (step_counter.cpp).
COUNTERS = {
    0x8065AA6C,  # system_w+0xC: steps (RunTasks)
    0x8065B2F4,  # system_w+0x894: play-time steps (0x800D2F80)
    0x806ACFF4,  # village work+0x30: steps (0x80212680)
    0x806ACE98,  # village talk work+0x150: steps (0x8021DC88)
    0x806C04E8,  # steps in the area (0x802E4C88, step_counter.cpp)
    0x806C02D8,  # (0x802E4C88)
    0x806C046C,  # (0x802E4C88)
    0x806C7D00,  # quest work+0x28: quest clock steps (0x803B0228, tick sites)
}
# Reads of the counters above through a base register (a work pointer), which
# the lis/addi tracking cannot see: pc -> counter.
POINTER_READS = {
    0x803B0C0C: 0x806C7D00,  # quest clock, right after its increment (0x803B0C00)
    0x803B0CF0: 0x806C7D00,  # (0x803B0CE4)
}
# Sites in functions ported to native C, which test the counter themselves
# (StepCounter::Every): pc -> the port.
PORTED = {
    0x8011E764: 'enemy_timers.cpp',  # 0x8011E750
    0x80130D60: 'enemy_timers.cpp',  # 0x80130D4C
    0x801312DC: 'enemy_timers.cpp',  # 0x801311B4
    0x80132D80: 'enemy_timers.cpp',  # 0x80132D50
    0x80133950: 'enemy_timers.cpp',  # 0x80133920
    0x80133B8C: 'enemy_timers.cpp',  # 0x80133B64
    0x801C89AC: 'enemy_timers.cpp',  # 0x801C8978
    0x8035C010: 'enemy_timers.cpp',  # 0x8035BFC8
    0x8038CCB8: 'enemy_timers.cpp',  # 0x8038CC84
}
# Periodic tests given by hand (the lis/addi tracking cannot see them, or
# site_kind cannot parse them): pc -> (N, k, kind). Kind 2: the player's step
# counter (PLW+0x20, advanced a tick at a time by player_timers.cpp) read through
# the PLW in code that runs once a step. The `& mask` tests whose mask is 15 or
# 63 at run time take N = 16: with fewer than 16 ticks in a step, the latest
# multiple of 16 the step passed is the multiple of 64 when it passed one.
MANUAL = {
    0x801514B4: (8, 4, 1),   # 0x80151460: `steps & 4`, after its `steps % 4 == 0` (0x80151488)
    0x802645B0: (30, 0, 2),  # 0x80264334 (once a step, after PlayerStepTimers): condition 1 effect
    0x8026461C: (32, 0, 2),  # 0x80276514 by 0x8027D470's amount
    0x80264678: (60, 0, 2),  # 0x80276514 by 0x80260A7C's amount
    0x802648D0: (8, 0, 2),   # 0x80276514 with cat skill 47
    0x8026491C: (16, 0, 2),  # effect 0x801005FC 39
    0x808D1140: (32, 0, 2),  # quest_field pl_effect_sub 0x808D10F0: condition 128 effect
    0x808D11B0: (16, 0, 2),  # damage condition 16, mask 63 or 15
    0x808D11E8: (16, 0, 2),  # damage condition 32
    0x808D1220: (16, 0, 2),  # damage condition 256
    0x808D1258: (16, 0, 2),  # damage condition 512
    0x808D129C: (32, 0, 2),  # damage conditions from the table at 0x80918740
    0x808D12E0: (36, 0, 2),  # condition 0x20000
}
# Their own increments (lwz; addi 1; stw), not reads.
INCREMENTS = {0x80041754, 0x800D2F88, 0x80212730, 0x8021DD04, 0x802E4C90, 0x802E4CA4, 0x802E4CB0,
              0x803B0C00, 0x803B0CE4}

def parse(path):
    lines = []
    for l in open(path):
        m = re.match(r'^([0-9a-f]{8}):\s+(\S+)\s*(.*)$', l.rstrip())
        if m:
            lines.append((int(m[1], 16), m[2], [o.strip() for o in m[3].split(',')] if m[3] else []))
        elif re.match(r'^[0-9a-f]{8} <', l):
            lines.append(None)
    return lines

def site_kind(lines, i, reg):
    """Returns (N, k, eq) for the periodic test on the counter loaded into reg at i, or None."""
    n = k = None
    val = reg
    j = i + 1
    end = min(i + 14, len(lines))
    while j < end and lines[j] is not None:
        pc, op, ops = lines[j]
        if op in ('b', 'blr', 'bctr'):
            return None
        if op == 'clrlwi' and ops[1] == val:
            n = 1 << (32 - int(ops[2])); val = ops[0]
        elif op == 'divwu' and ops[1] == val and n is None:
            n = 0  # N from a register: only its == 0 test is answerable (N = 0 in the table)
        elif op == 'mulli' and n is None:
            n = int(ops[2]) if ops[1] != val else None
        elif op in ('sub', 'subf') and n is not None and (ops[1] == val or ops[2] == val):
            val = ops[0]
        elif op in ('cmplwi', 'cmpwi') and len(ops) >= 2 and ops[-2] == val:
            k = int(ops[-1])
            cr = ops[0] if len(ops) == 3 else '0'
            # the branch testing this compare
            for jj in range(j + 1, min(j + 4, end)):
                if lines[jj] is None:
                    break
                bop, bops = lines[jj][1], lines[jj][2]
                # bt/bf, or the raw `bc BO, BI` llvm-objdump prints for them
                # with a prediction hint (BO 5, 13): a CR bit test without CTR.
                if bop == 'bc' and len(bops) >= 2 and bops[0].isdigit() and int(bops[0]) & 0x14 == 0x04:
                    bop, bops = 'bt', bops[1:]
                if bop in ('bt', 'bf') and bops and bops[0].isdigit():
                    bit = int(bops[0])
                    eq = bit % 4 == 2 and bit // 4 == int(cr.replace('cr', '') or 0)
                    return n, k, eq
            return n, k, False
        elif op == 'cmplw' or op == 'cmpw':
            return None
        j += 1
    return None

def main():
    lines = parse(sys.argv[1])
    owner, shared = {}, set()
    for path in sys.argv[2:]:
        for e in parse(path):
            if e is not None and e[0] < 0x80800000:
                continue
            if e is not None and owner.setdefault(e[0], path) != path:
                shared.add(e[0])
            lines.append(e)
        lines.append(None)
    sites, skipped = [], []
    # Registers holding a lis/addi address, per function (as addrfind.py).
    reg = {}
    for i, e in enumerate(lines):
        if e is None:
            reg = {}
            continue
        pc, op, ops = e
        if op == 'lis':
            reg[ops[0]] = (int(ops[1]) << 16) & 0xFFFFFFFF
            continue
        if op == 'addi' and len(ops) == 3 and ops[1] in reg:
            reg[ops[0]] = (reg[ops[1]] + int(ops[2])) & 0xFFFFFFFF
            continue
        m = re.match(r'(-?\d+)\((\d+)\)$', ops[-1]) if ops else None
        if op == 'lwz' and m and (m[2] in reg or pc in POINTER_READS) and pc not in INCREMENTS and pc not in PORTED and pc not in MANUAL:
            if pc in POINTER_READS or (reg[m[2]] + int(m[1])) & 0xFFFFFFFF in COUNTERS:
                got = site_kind(lines, i, ops[0])
                if got and (got[0] or (got[0] == 0 and got[1] == 0 and got[2])):
                    sites.append((pc, got[0], got[1], 1 if got[2] else 0))
                else:
                    skipped.append(pc)
        if ops and ops[0] in reg and op not in ('stw', 'stb', 'sth', 'stfs', 'stfd', 'cmpwi', 'cmplwi', 'cmpw', 'cmplw'):
            reg.pop(ops[0], None)
    sites += [(pc, n, k, kind) for pc, (n, k, kind) in MANUAL.items()]
    sites = sorted(set(sites))
    for site in sites:
        if site[0] in shared:
            raise SystemExit(f'site {site[0]:08x} is in more than one overlay')
    out = ['// Generated by scripts/step_counter_sites.py; do not edit.',
           '// {pc, N, k, kind}: the step counter read at pc is tested as counter % N (== k: kind 1,',
           "// or 2 on a player's step counter);",
           '// N = 0: N is only known at run time and the test is == 0.']
    out += [f'{{0x{pc:08X}u, {n}u, {k}u, {eq}}},' for pc, n, k, eq in sites]
    open('runtime/src/step_counter_sites.inc', 'w').write('\n'.join(out) + '\n')
    print(f'{len(sites)} periodic sites ({sum(s[3] > 0 for s in sites)} equality), {len(skipped)} other reads', file=sys.stderr)
    for pc in skipped:
        print(f'  other read at {pc:08x}', file=sys.stderr)

main()
