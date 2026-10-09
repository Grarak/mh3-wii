"""The tick sites one translation hooks (runtime/src/tick_sites.inc).

A boot module and an RSO overlay can share addresses (demo_data and net_data
both load at 0x8097D080), so a site there names the module whose code it is
and only that module's translation may hook the load at its pc.
"""
import re
from link_rso import ROOT

SOURCE = ROOT / 'runtime/src/tick_sites.inc'
MODULE = re.compile(r'^\{0x[0-9A-Fa-f]+u,[^}]*"(\w+)"\s*\}')


def write_filtered(path, modules):
    """Write the sites that are untagged or tagged with one of `modules` to
    `path` (only if it changed, to keep its mtime); returns the path."""
    lines = []

    for line in SOURCE.read_text().splitlines():
        tag = MODULE.match(line)

        if not tag or tag.group(1) in modules:
            lines.append(line)

    text = '\n'.join(lines) + '\n'

    if not path.exists() or path.read_text() != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    return path
