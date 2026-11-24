#!/usr/bin/env python3
"""Lists undocumented public declarations in the engine's headers.

Runs Doxygen (the API reference's configuration, docs/api/Doxyfile.in) with
"warn if undocumented" on, and reports per header.

    python3 tools/check_docs.py                # summary: undocumented count per header
    python3 tools/check_docs.py --list FILE..  # every undocumented declaration in those headers
    python3 tools/check_docs.py --changed      # only headers changed vs origin/main (or --base REF):
                                               # exit 1 if any of them has undocumented declarations

Run --changed before committing: a header you touched must come out fully
documented (docs/03-conventions.md, "Code documentation"). Needs doxygen.
"""
import argparse
import collections
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER_DIRS = ["engine/src", "effects/src"]


def run_doxygen(out_dir):
    doxygen = shutil.which("doxygen")
    if not doxygen:
        sys.exit("check_docs: doxygen not found (install it: apt install doxygen / winget install doxygen / brew install doxygen)")
    template = (ROOT / "docs/api/Doxyfile.in").read_text()
    values = {
        "PROJECT_VERSION": "check",
        "VE_DOCS_OUTPUT": str(out_dir),
        "PROJECT_SOURCE_DIR": str(ROOT),
        "VE_DOCS_THEME": str(out_dir),  # (no HTML wanted)
    }
    for k, v in values.items():
        template = template.replace("@" + k + "@", v)
    # Headers only, no HTML: just the warnings.
    template += "\nINPUT = " + " ".join(str(ROOT / d) for d in HEADER_DIRS) + "\nFILE_PATTERNS = *.hpp\n"
    template += "GENERATE_HTML = NO\nHTML_EXTRA_STYLESHEET =\nUSE_MDFILE_AS_MAINPAGE =\n"
    doxyfile = pathlib.Path(out_dir) / "Doxyfile"
    doxyfile.write_text(template)
    env = dict(os.environ, VE_DOCS_EXTRACT_ALL="NO", VE_DOCS_WARN_UNDOCUMENTED="YES")
    subprocess.run([doxygen, str(doxyfile)], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return (pathlib.Path(out_dir) / "warnings.log").read_text(errors="replace")


def parse(log):
    """{header (repo-relative): [(line, message)]} for undocumented declarations."""
    found = collections.defaultdict(list)
    pattern = re.compile(r"^(.*?):(\d+): warning: (.*)$")
    for line in log.splitlines():
        m = pattern.match(line)
        if not m or "is not documented" not in m.group(3):
            continue
        try:
            rel = pathlib.Path(m.group(1)).resolve().relative_to(ROOT).as_posix()
        except ValueError:
            continue
        found[rel].append((int(m.group(2)), m.group(3)))
    # Inherited members are reported once per derived class: one entry per line.
    for rel, items in found.items():
        seen, unique = set(), []
        for line, msg in sorted(items):
            if line not in seen:
                seen.add(line)
                unique.append((line, msg))
        found[rel] = unique
    return found


def changed_headers(base):
    def git(*args):
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.split()

    files = set(git("diff", "--name-only", base + "...HEAD")) | set(git("diff", "--name-only")) | set(git("diff", "--name-only", "--cached"))
    files |= set(git("ls-files", "--others", "--exclude-standard"))
    return sorted(f for f in files if f.endswith(".hpp") and any(f.startswith(d + "/") for d in HEADER_DIRS) and (ROOT / f).exists())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--changed", action="store_true", help="only headers changed vs --base (plus uncommitted ones); fail if undocumented")
    ap.add_argument("--base", default="origin/main", help="git ref for --changed (default origin/main)")
    ap.add_argument("--list", nargs="*", metavar="FILE", help="list every undocumented declaration (in these headers, or all)")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        found = parse(run_doxygen(tmp))

    if args.changed:
        files = changed_headers(args.base)
        bad = {f: found[f] for f in files if found.get(f)}
        for f, items in sorted(bad.items()):
            for line, msg in sorted(items):
                print(f"{f}:{line}: {msg}")
        print(f"check_docs: {len(files)} changed header(s), {sum(len(v) for v in bad.values())} undocumented declaration(s)")
        return 1 if bad else 0

    if args.list is not None:
        want = set(args.list) if args.list else None
        for f in sorted(found):
            if want and f not in want:
                continue
            for line, msg in sorted(found[f]):
                print(f"{f}:{line}: {msg}")
        return 0

    total = sum(len(v) for v in found.values())
    headers = sorted(set(str(p.relative_to(ROOT).as_posix()) for d in HEADER_DIRS for p in (ROOT / d).rglob("*.hpp")))
    clean = [h for h in headers if not found.get(h)]
    for f in sorted(found, key=lambda f: -len(found[f])):
        print(f"{len(found[f]):5}  {f}")
    print(f"check_docs: {total} undocumented declaration(s) in {len(found)} header(s); {len(clean)} of {len(headers)} headers fully documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
