#!/usr/bin/env python3
"""Aggregate macOS sample call trees without double-counting recursive callers.

Percentages use samples on the selected thread, including waits. Self counts
subtract direct children; inclusive counts count each stack at most once per
function. This is sampled wall time, not instrumented CPU/GPU timing.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def summarize(path, thread_match='com.apple.main-thread'):
    roots, stack = [], []
    tree = path.read_text().split('Call graph:\n', 1)[1].split('\nTotal number', 1)[0]
    for line in tree.splitlines():
        match = re.match(r'^([ +!:|]*)(\d+) (.+)', line)
        if not match:
            continue
        depth = match.start(2)
        name = re.split(r'  \(in |  \[0x', match[3], maxsplit=1)[0]
        node = {'count': int(match[2]), 'name': name, 'children': [], 'depth': depth}
        while stack and stack[-1]['depth'] >= depth:
            stack.pop()
        if stack:
            stack[-1]['children'].append(node)
        else:
            roots.append(node)
        stack.append(node)
    selected = [r for r in roots if thread_match in r['name']]
    if not selected:
        raise ValueError(f'No thread matching {thread_match!r}')
    own, inclusive = Counter(), Counter()
    def walk(node, ancestors):
        children = node['children']
        count = node['count'] - sum(c['count'] for c in children)
        if count < 0:
            raise ValueError(f'Invalid tree under {node["name"]}')
        own[node['name']] += count
        for name in set(ancestors + [node['name']]):
            inclusive[name] += count
        for child in children:
            walk(child, ancestors + [node['name']])
    for root in selected:
        walk(root, [])
    total = sum(r['count'] for r in selected)
    assert sum(own.values()) == total
    return {'samples': total, 'threads': [r['name'] for r in selected],
            'functions': [{'name': n, 'self': own[n], 'inclusive': inclusive[n],
                           'self_percent': round(100 * own[n] / total, 2),
                           'inclusive_percent': round(100 * inclusive[n] / total, 2)}
                          for n in sorted(own, key=lambda n: (-own[n], n))]}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sample', type=Path)
    parser.add_argument('--thread', default='com.apple.main-thread')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    result = summarize(args.sample, args.thread)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f'{result["samples"]} samples; self% / inclusive% / function')
        for row in result['functions'][:35]:
            print(f'{row["self_percent"]:6.2f} {row["inclusive_percent"]:6.2f} {row["name"]}')
