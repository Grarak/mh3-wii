#!/usr/bin/env python3
"""Generate runtime/src/language_japanese_map.inc from the two discs.

The European build keeps its text in archives, the Japanese one in its overlays.
Both come from the same tables, so every European string has a Japanese partner
at a pointer slot of an overlay. This writes those pairs as runs of numbers: no
game text goes into the file, the import reads the strings from the user's disc.

usage: language_map.py Assets/DATA/files <extracted RMHJ08 files> runtime/src/language_japanese_map.inc
"""
import argparse
import bisect
import importlib.util
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('link_rso', ROOT / 'scripts/link_rso.py')
link_rso = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link_rso)

# The Japanese executable's arrays of the system and save messages.
SYSTEM_MESSAGES, SAVE_MESSAGES = 0x806C5CF0, 0x806C5D50

# In a run's overlay number: the strings are scrambled (each byte inverted),
# or start with a player.
SCRAMBLED, PLAYER_1, PLAYER_2 = 8, 16, 32

# The Japanese executable's index of the guide's lines, and their second part.
GUIDE_INDEX, GUIDE_LATER = 0x806B10A8, 0x806B0FC0

# (archive, overlay of its Japanese text)
MODULES = {'com_data': 0, 'lobby_data': 1, 'quest_data': 2, 'demo_data': 3, 'net_data': 4, 'main': 5}


def omom(data):
    count = struct.unpack('>I', data[4:8])[0]
    return [struct.unpack('>II', data[8 + i * 8:16 + i * 8]) for i in range(count)]


def word(data, at):
    return struct.unpack('>I', data[at:at + 4])[0]


def text_at(data, at):
    return data[at:data.index(b'\0', at)]


def decoded(raw):
    """Shift-JIS text, or None for other bytes: data looks like it now and then."""
    try:
        text = raw.decode('cp932')
    except UnicodeDecodeError:
        return None

    return None if any(ord(c) < 0x20 and c not in '\n\t' or 0xE000 <= ord(c) < 0xF900 for c in text) else text


class Overlay:
    """A Japanese overlay: its ADDR32 pointer slots in file order, named by its exports."""

    def __init__(self, path):
        self.data = path.read_bytes()
        rso = link_rso.Rso(self.data)
        self.sections = rso.sections
        self.target = {}

        for offset, info, addend in rso.internal:
            section, kind = info >> 8, info & 255
            start = self.sections[section][0] if section < len(self.sections) else 0

            if start and kind == 1:
                self.target[offset] = start + addend

        self.slots = sorted(self.target)
        self.position = {slot: i for i, slot in enumerate(self.slots)}
        self.symbols = {}

        for name, offset, section in rso.exports:
            if not name.startswith('.') and section < len(self.sections) and self.sections[section][0]:
                self.symbols.setdefault(name, self.sections[section][0] + offset)

    def string(self, slot):
        return decoded(text_at(self.data, self.target[slot]))


def features(text):
    numbers = re.findall(r'\d+', text.translate({c: c - 0xFEE0 for c in range(0xFF10, 0xFF1A)}))
    return (text.count('\n'), sorted(re.findall(r'%[-+0-9.]*[a-zA-Z]', text)), sorted(re.findall(r'~[A-Z]\d*', text)), numbers)


def similar(english, japanese):
    if japanese is None:
        return -6
    if not english or not japanese:
        return 3 if english == japanese else -3

    e, j = features(english), features(japanese)
    score = 2 if e[0] == j[0] else -min(abs(e[0] - j[0]), 3)
    score += (4 if e[1] else 0) if e[1] == j[1] else -4
    score += (3 if e[2] else 0) if e[2] == j[2] else -3
    score += (3 if e[3] else 0) if e[3] == j[3] else (-1 if e[3] and j[3] else 0)
    ratio = len(english) / (len(japanese) * 2)
    score += 1 if 0.35 < ratio < 2.5 else -2
    return score


def tables(entry):
    first = word(entry, 0)
    return [struct.unpack('>II', entry[i * 8:i * 8 + 8]) for i in range(first // 8)]


def strings_of(entry, table, count):
    return [text_at(entry, word(entry, table + i * 4)).decode('utf8') if word(entry, table + i * 4) else '' for i in range(count)]


class Map:
    """European string references as runs: the entry's word (stepping word_step)
    against the Japanese slot (stepping slot_step)."""

    def __init__(self):
        self.runs = []
        self.total = 0

    def add(self, name, entry, word_index, module, slot, count, word_step=1, slot_step=4):
        self.total += count
        last = self.runs[-1] if self.runs else None

        if last and last[:2] == [name, entry] and last[3:5] == [word_step, module] and last[6] == slot_step:
            if last[2] + last[7] * word_step == word_index and last[5] + last[7] * slot_step == slot:
                last[7] += count
                return

        self.runs.append([name, entry, word_index, word_step, module, slot, slot_step, count])


def by_symbols(result, name, entry_index, entry, overlay, module, slots):
    for (table, count), slot in zip(tables(entry), slots):
        result.add(name, entry_index, table // 4, module, slot, count)


def best_place(english, slots, strings, at):
    """Where in the slots the strings suit best; the slot after the last table
    is the first choice."""
    count = len(english)
    best, place = None, None

    for p in range(0, len(slots) - count + 1):
        if slots[p + count - 1] != slots[p] + (count - 1) * 4 or (strings[p] is None and p != at):
            continue

        score = sum(similar(e, strings[p + k]) for k, e in enumerate(english)) / max(count, 1)
        score += 2.5 if p == at else 0
        score -= 0.002 * abs(p - at) if count < 8 else 0

        if best is None or score > best:
            best, place = score, p

    return best, place


def japanese_only(overlay, slots):
    # Only text with Japanese in it can be a partner of a translated string.
    strings = []

    for slot in slots:
        text = overlay.string(slot)
        strings.append(text if text is not None and (not text or not text.isascii() or '%' in text) else None)

    return strings


def sequence(result, name, entry_index, entry, overlay, module, start, end, report, chosen=None):
    """The tables of an entry against the slots of [start, end), in order."""
    slots = [s for s in overlay.slots if start <= s < end]
    strings = japanese_only(overlay, slots)
    at = 0
    listing = tables(entry)

    for index, (table, count) in enumerate(listing):
        if chosen is not None and index not in chosen:
            continue

        english = strings_of(entry, table, count)
        best, place = best_place(english, slots, strings, at)

        if place is None:
            report.append(f'{name}.{entry_index} t{index} n={count} UNPLACED')
            continue

        result.add(name, entry_index, table // 4, module, slots[place], count)
        report.append(f'{name}.{entry_index} t{index} n={count} score {best:.1f} {slots[place]:#x}: {english[0][:34]!r} = {(strings[place] or "")[:18]!r}')
        at = place + count


def walk(result, name, entry_index, entry, overlay, module, slot, chosen):
    """The tables in the overlay's order from `slot`, whatever lies between them
    that is not text."""
    listing = tables(entry)

    for index in chosen:
        table, count = listing[index]

        while slot not in overlay.target or not overlay.string(slot):
            slot += 4

        result.add(name, entry_index, table // 4, module, slot, count)
        slot += count * 4


def guide(result, entry, executable):
    """The guide's lines of the first quests, data00's entry 6: arrays in the
    Japanese executable. An index gives the first ones, zeros between; the
    second part's follow each other."""
    listing = tables(entry)
    at = GUIDE_INDEX

    for table, count in listing[:36]:
        while not word(executable.data, executable.file_offset(at)):
            at += 4

        result.add('data00', 6, table // 4, MODULES['main'], word(executable.data, executable.file_offset(at)), count)
        at += 4

    slot = GUIDE_LATER

    for table, count in listing[36:47]:
        result.add('data00', 6, table // 4, MODULES['main'], slot, count)
        slot += count * 4


def rows(result, name, entry_index, entry, overlay, module, report):
    """data10's u32 count, then count rows of pointers: entry 0 three, entry 2 seven."""
    count = word(entry, 0)
    width = {0: 3, 2: 7}[entry_index]
    total = count * width
    english = [text_at(entry, word(entry, 4 + i * 4)).decode('utf8') for i in range(total)]
    slots = overlay.slots
    strings = japanese_only(overlay, slots)
    best, place = best_place(english, slots, strings, 0)
    result.add(name, entry_index, 1, module, slots[place], total)
    report.append(f'{name}.{entry_index} rows {count}x{width} score {best:.1f} {slots[place]:#x}: {english[0][:30]!r} = {(strings[place] or "")[:18]!r}')


def talk(result, name, entry_index, entry, overlay, module, europe, descriptor, report):
    """Talk groups: five (table, count) pairs, whose destinations are records
    {u32, char*} under one name in both overlays."""
    groups = struct.unpack('>64I', entry[:256])
    placed = missing = 0

    for i, group in enumerate(groups):
        if not group:
            continue

        first, more = europe.target.get(descriptor + i * 8), europe.target.get(descriptor + i * 8 + 4)
        destinations = [first] + [europe.target.get(more + k * 8) if more else None for k in range(4)]

        for pair, destination in enumerate(destinations):
            table, count = struct.unpack('>II', entry[group + pair * 8:group + pair * 8 + 8])

            if not table or not count:
                continue

            symbol = europe.names.get(destination)
            slot = overlay.symbols.get(symbol) if symbol else None

            if slot is None:
                missing += count
                report.append(f'{name}.{entry_index} group {i} pair {pair} n={count} no partner for {symbol}')
                continue

            for k in range(count):
                if word(entry, table + k * 4) and slot + k * 8 + 4 in overlay.target:
                    result.add(name, entry_index, table // 4 + k, module, slot + k * 8 + 4, 1, slot_step=8)
                    placed += 1
                elif word(entry, table + k * 4):
                    missing += 1

    report.append(f'{name}.{entry_index} talk placed {placed} missing {missing}')


def quest_lines(result, name, data, overlay, module, report):
    """data20: each entry a list of {u32 id, u32 text}, q_npc_talk_dataNN in the overlay."""
    for index, (offset, size) in enumerate(omom(data)):
        entry = data[offset:offset + size]
        slot = overlay.symbols[f'q_npc_talk_data{index:02}']
        result.add(name, index, 1, module, slot + 4, word(entry, 4) // 8, word_step=2, slot_step=8)


class Executable:
    """The Japanese main.dol as a source: its data words that point at strings."""

    def __init__(self, path):
        self.data = path.read_bytes()
        offsets = struct.unpack_from('>18I', self.data, 0)
        addresses = struct.unpack_from('>18I', self.data, 0x48)
        sizes = struct.unpack_from('>18I', self.data, 0x90)
        self.ranges = [(addresses[i], sizes[i], offsets[i]) for i in range(18) if sizes[i]]
        self.target = {}

        for address, size, offset in self.ranges[1:] if False else [r for i, r in enumerate(self.ranges) if r[0] >= 0x80000000 and i >= 2]:
            for at in range(0, size & ~3, 4):
                value = word(self.data, offset + at)
                position = self.file_offset(value)

                if position is not None and (self.data[position - 1] == 0 or (value & 3) == 0) and self.data[position] >= 0x20:
                    self.target[address + at] = position

        self.slots = sorted(self.target)

    def file_offset(self, address):
        for start, size, offset in self.ranges:
            if start <= address < start + size:
                return offset + address - start

        return None

    def string(self, slot):
        return decoded(text_at(self.data, self.target[slot]))


class EuropeOverlay(Overlay):
    def __init__(self, path):
        super().__init__(path)
        self.names = {at: name for name, at in self.symbols.items()}


# demo_data's menus the Japanese overlay has under no name, by slot.
MENU_SLOTS = {'title_mychar_menu_msg': 0x22540, 'title_mychar_pl_msg': 0x22548, 'title_mychar_add_msg': 0x22550, 'title_mychar_same_msg': 0x22560, 'title_file_name_msg': 0x22468,
              'title_mychar_guide_msg_1p': 0x22558, 'title_mychar_guide_msg_2p': 0x22558}

# The Japanese game writes the player before these two.
MENU_PLAYERS = {'title_mychar_guide_msg_1p': PLAYER_1, 'title_mychar_guide_msg_2p': PLAYER_2}


def menus(result, europe, overlay):
    """demo_data's menus: name_eng here, name there. The word is the European
    slot's, in the overlay's file. The gallery's text is scrambled like the
    quest files'."""
    for name, slot in sorted(europe.symbols.items(), key=lambda s: s[1]):
        if not name.endswith('_eng'):
            continue

        count = 0

        while slot + count * 4 in europe.target and (count == 0 or slot + count * 4 not in europe.names):
            count += 1

        partner = overlay.symbols.get(name[:-4], MENU_SLOTS.get(name[:-4]))
        scrambled = name.startswith('gallery_') and name != 'gallery_daynight_msg_eng'

        if partner is not None:
            result.add('demo_data', 0, slot // 4, MODULES['demo_data'] | (SCRAMBLED if scrambled else 0) | MENU_PLAYERS.get(name[:-4], 0), partner, count)


class Utf8Overlay(EuropeOverlay):
    def string(self, slot):
        try:
            return text_at(self.data, self.target[slot]).decode('utf8')
        except UnicodeDecodeError:
            return None


def write(path, result):
    names = ['data00', 'data10', 'data11', 'data20', 'demo_data', 'main']
    out = ['// Generated by scripts/language_map.py: where the Japanese disc has the partner',
           '// of each European string. Numbers only; the import reads the text from the disc.',
           '// {archive, entry, word, word step, overlay, slot, slot step, count}']

    for run in sorted(result.runs, key=lambda r: (names.index(r[0]), r[1], r[2])):
        out.append(f'{{{names.index(run[0])}, {run[1]}, {run[2]}, {run[3]}, {run[4]}, {run[5]:#x}, {run[6]}, {run[7]}}},')

    path.write_text('\n'.join(out) + '\n')


def write_sjis(path):
    """JIS X 0208's rows as Unicode, for the Japanese disc's text and font."""
    out = ['// Generated by scripts/language_map.py: Unicode of each JIS cell, row by row (94 cells a row).']

    for row in range(94):
        codes = []

        for cell in range(94):
            lead, trail = (row >> 1) + 0x81, cell + (0x9F if row & 1 else 0x40 + (cell >= 63))
            lead += 0x40 if lead > 0x9F else 0

            try:
                codes.append(ord(bytes([lead, trail]).decode('cp932')))
            except (UnicodeDecodeError, TypeError):
                codes.append(0)

        out.append(', '.join(f'{code:#x}' for code in codes) + ',')

    path.write_text('\n'.join(out) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('europe', type=Path)
    parser.add_argument('japan', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    write_sjis(args.output.with_name('language_jis.inc'))
    com = Overlay(args.japan / 'rel/com_data.rso')
    lobby = Overlay(args.japan / 'rel/lobby_data.rso')
    quest = Overlay(args.japan / 'rel/quest_data.rso')
    demo = Overlay(args.japan / 'rel/demo_data.rso')
    europeLobby = EuropeOverlay(args.europe / '01/lobby_data.rso')
    europeDemo = Utf8Overlay(args.europe / '01/demo_data.rso')
    result = Map()
    report = []
    data00 = (args.europe / '00/data00_eng.bin').read_bytes()
    entries = omom(data00)
    entry = lambda i: data00[entries[i][0]:entries[i][0] + entries[i][1]]
    symbol = lambda *names: [com.symbols[n] for n in names]
    module = MODULES['com_data']
    by_symbols(result, 'data00', 0, entry(0), com, module, symbol('item_str', 'item_exp'))
    by_symbols(result, 'data00', 1, entry(1), com, module,
               symbol('lsword_str', 'sword_str', 'hammer_str', 'lsword2_str', 'axe_str', 'lance_str', 'lsword_exp', 'sword_exp', 'hammer_exp', 'lsword2_exp', 'axe_exp', 'lance_exp'))
    by_symbols(result, 'data00', 2, entry(2), com, module, symbol('Frame_str', 'Stock_str', 'Barrel_str', 'Frame_exp', 'Stock_exp', 'Barrel_exp'))
    armor = [com.target[com.symbols['armor_str'] + i * 4] for i in range(6)] + [com.target[com.symbols['armor_explain'] + i * 4] for i in range(6)]
    by_symbols(result, 'data00', 3, entry(3), com, module, armor)
    region = com.symbols['lance_exp'] + 98 * 4
    sequence(result, 'data00', 4, entry(4), com, module, region, com.symbols['tuto_t00_f00_s00_msg'], report, range(76))
    tutorials = sorted((at, n) for n, at in com.symbols.items() if n.startswith('tuto_') and n != 'tuto_message_control_code')
    listing = tables(entry(4))

    for (table, count), (at, _) in zip(listing[76:391], tutorials):
        result.add('data00', 4, table // 4, module, at, count)

    sequence(result, 'data00', 4, entry(4), com, module, 0, 1 << 30, report, range(391, 398))
    sequence(result, 'data00', 5, entry(5), com, module, 0, 1 << 30, report, range(23))
    walk(result, 'data00', 5, entry(5), com, module, com.symbols['menu_top_help_message0_ed'], range(23, 55))
    sequence(result, 'data00', 5, entry(5), com, module, com.symbols['menu_chogo_help_message'], 1 << 30, report, range(55, 74))
    executable = Executable(args.japan.parent / 'sys/main.dol')

    guide(result, entry(6), executable)
    module = MODULES['lobby_data']
    symbols = lobby.symbols

    for name in ('data10', 'data11'):
        data = (args.europe / f'00/{name}_eng.bin').read_bytes()
        entries10 = omom(data)
        part = lambda i: data[entries10[i][0]:entries10[i][0] + entries10[i][1]]
        rows(result, name, 0, part(0), lobby, module, report)
        listing = tables(part(1))
        plain = {0: 'lb_shop_str', 1: 'lb_etc_str', 2: 'lb_npc_name_tbl', 3: 'lb_help_str', 4: 'lb_unique_str', 5: 'lb_pig_name_tbl', 6: 'lb_cat_name_tbl',
                 7: 'lb_chacha_mask_str_tbl', 8: 'lb_chacha_skill_str_tbl', 11: 'lbex_main_str_tbl', 12: 'lbex_zaco_str_tbl', 13: 'jump_err_msg'}

        for index, symbol in plain.items():
            result.add(name, 1, listing[index][0] // 4, module, symbols[symbol], listing[index][1])

        # The boats' upgrades: {u32, name, text}.
        result.add(name, 1, listing[9][0] // 4, module, symbols['lb_tr_flag_data_tbl'] + 4, listing[9][1], slot_step=12)
        result.add(name, 1, listing[10][0] // 4, module, symbols['lb_tr_flag_data_tbl'] + 8, listing[10][1], slot_step=12)

        # The boxes: {u32, seven strings}.
        for row in range(word(part(2), 0)):
            result.add(name, 2, 1 + row * 7, module, symbols['lb_item_get_data_tbl'] + row * 32 + 4, 7)

        talk(result, name, 3, part(3), lobby, module, europeLobby, 0x809225C8 - 0x808C76A0, report)

    quest_lines(result, 'data20', (args.europe / '00/data20_eng.bin').read_bytes(), quest, MODULES['quest_data'], report)
    menus(result, europeDemo, demo)

    # The system and save messages: same order, same length.
    result.add('main', 0, 0, MODULES['main'], SYSTEM_MESSAGES, 24)
    result.add('main', 1, 0, MODULES['main'], SAVE_MESSAGES, 9)
    print('pairs', result.total, 'runs', len(result.runs))

    write(args.output, result)


if __name__ == '__main__':
    main()
