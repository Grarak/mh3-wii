#!/usr/bin/env python3
"""Add the blank lines clang-format and CMake leave out.

  space_code.py FILE... [--check]

C and C++:

  * a blank line between definitions (functions, classes, namespaces' ends)
    and before a comment that follows code at the top level; a function's
    REGISTER_* line stays with it
  * a blank line before a comment that starts a new step inside a block
  * a blank line before a control statement (if, for, while, switch, try, do)
    that follows other statements
  * a blank line after a closed block, or a control statement's body without
    braces, followed by another statement

CMake:

  * a blank line before a comment and before a block (if, foreach, while,
    function, macro) that follows other commands
  * a blank line after a block's end followed by another command

Never inside a macro continued with a backslash, a raw string literal or a
CMake bracket argument. With --check, lists the files that would change and
exits 1 if any would.
"""

import re
import sys
from pathlib import Path

CPP = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".mm"}
# Lines after a closing brace that continue its statement or close another block.
CONTINUES = re.compile(r"(\}|\)|;|,|else\b|catch\b|while\b|case\b|default\b|#|public:|private:|protected:|//|/\*|\*/|\*\s|\*$)")


def indent(line):
    return len(line) - len(line.lstrip())


def cpp_protected(lines):
    """Indices of lines inside raw strings, block comments or backslash-continued macros."""
    protected, raw_end, in_comment = set(), None, False
    for i, line in enumerate(lines):
        if raw_end is not None:
            protected.add(i)
            if raw_end in line:
                raw_end = None
            continue
        if in_comment:
            protected.add(i)
            if "*/" in line:
                in_comment = False
            continue
        match = re.search(r'R"([^(\s]*)\(', line)
        if match and f'){match.group(1)}"' not in line[match.end():]:
            raw_end = f'){match.group(1)}"'
        stripped = line.strip()
        if stripped.startswith("/*") and "*/" not in stripped:
            in_comment = True
        if i and lines[i - 1].rstrip().endswith("\\"):
            protected.add(i)
    return protected


CONTROL = re.compile(r"(if|for|while|switch)\s*\(|(try|do)\s*\{|if constexpr\s*\(")
HEADER = re.compile(r"((\}\s*)?else\s+)?(if|for|while)\s*\(.*\)\s*$|(\}\s*)?else\s*$")


def space_cpp(text):
    lines = text.split("\n")
    protected = cpp_protected(lines)
    out = []
    # The line before the previous one at the current line's indentation.
    for i, line in enumerate(lines):
        stripped = line.strip()
        previous = out[-1] if out else ""
        last = previous.strip()
        blank = False
        if i not in protected and last and stripped and indent(line) == 0 and indent(previous) == 0 and not previous.rstrip().endswith("\\"):
            # Top level: after a definition, and before a comment after code.
            closed = last.startswith("}") and not stripped.startswith(("}", "#", "REGISTER_", ")", ";", "else"))
            commented = stripped.startswith("//") and not last.startswith(("//", "#", "/*", "*")) and not last.endswith(("{", "*/"))
            blank = closed or commented
        elif i not in protected and last and stripped and indent(line) > 0 and not previous.rstrip().endswith("\\"):
            if indent(line) == indent(previous):
                statement = last.endswith(";") and not last.startswith("//")
                # A comment opening a new step after a statement or block.
                blank |= stripped.startswith("//") and (statement or last == "}")
                # A control statement after other statements.
                blank |= bool(CONTROL.match(stripped)) and statement and not last.startswith(("case ", "default:"))
                # A statement after a closed block.
                blank |= last in ("}", "};") and not CONTINUES.match(stripped)
            elif indent(line) < indent(previous) and len(out) >= 2:
                # A statement after a control statement's body without braces.
                header = out[-2]
                blank |= (indent(header) == indent(line) and HEADER.match(header.strip()) is not None and last.endswith(";")
                          and not CONTINUES.match(stripped))
        if blank:
            out.append("")
        out.append(line)
    return "\n".join(out)


CMAKE_BLOCK = re.compile(r"(if|foreach|while|function|macro|block)\s*\(", re.I)
CMAKE_END = re.compile(r"(endif|endforeach|endwhile|endfunction|endmacro|endblock)\s*\(", re.I)
CMAKE_MIDDLE = re.compile(r"(else|elseif)\s*\(", re.I)


def space_cmake(text):
    lines = text.split("\n")
    out, depth_paren, bracket = [], 0, None
    for line in lines:
        stripped = line.strip()
        previous = out[-1] if out else ""
        last = previous.strip()
        inside = depth_paren > 0 or bracket is not None
        if not inside and last and stripped and indent(line) == indent(previous):
            command_before = not last.startswith("#") and not CMAKE_BLOCK.match(last) and not CMAKE_MIDDLE.match(last)
            opens = stripped.startswith("#") or CMAKE_BLOCK.match(stripped)
            after_end = CMAKE_END.match(last) and not CMAKE_END.match(stripped) and not CMAKE_MIDDLE.match(stripped)
            if (opens and command_before) or after_end:
                out.append("")
        out.append(line)
        # Track multi-line command arguments and bracket arguments.
        if bracket is not None:
            if bracket in line:
                bracket = None
            continue
        match = re.search(r"\[(=*)\[", line)
        if match and f"]{match.group(1)}]" not in line[match.end():]:
            bracket = f"]{match.group(1)}]"
        code = re.sub(r'"(\\.|[^"\\])*"', '""', line.split("#", 1)[0] if not line.lstrip().startswith("#") else "")
        depth_paren = max(0, depth_paren + code.count("(") - code.count(")"))
    return "\n".join(out)


def main():
    check = "--check" in sys.argv
    changed = []
    for name in [a for a in sys.argv[1:] if a != "--check"]:
        path = Path(name)
        text = path.read_text()
        if path.suffix in CPP:
            spaced = space_cpp(text)
        elif path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            spaced = space_cmake(text)
        else:
            continue
        if spaced != text:
            changed.append(name)
            if not check:
                path.write_text(spaced)
    for name in changed:
        print(name)
    return 1 if check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
