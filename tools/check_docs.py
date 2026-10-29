"""Validate authored Markdown links and docs index coverage without network/device I/O.

Run from any directory. Standard-library only. Checks inline/reference links,
Markdown/HTML images, local heading fragments, and reachability from docs/README.
Code blocks, inline code, external URLs and vendored Markdown are excluded.
"""

import html
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def prose(text):
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if marker:
            token = marker[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            lines.append("")
        else:
            lines.append("" if fence else line)
    return "\n".join(lines)


def headings(text):
    anchors = set()
    counts = {}
    for line in prose(text).splitlines():
        match = re.match(r"^\s{0,3}#{1,6}\s+(.+?)(?:\s+#+)?$", line)
        if not match:
            continue
        title = re.sub(r"<[^>]+>", "", html.unescape(match[1])).lower()
        title = re.sub(r"[^\w\-\s]", "", title).replace(" ", "-")
        count = counts.get(title, 0)
        counts[title] = count + 1
        anchors.add(title + (f"-{count}" if count else ""))
    anchors.update(re.findall(r'<[^>]+\b(?:id|name)=[\'\"]([^\'\"]+)', text))
    return anchors


def links(text):
    text = prose(text)
    # Hide inline code (including examples which look like links).
    text = re.sub(r"(`+).*?\1", "", text)
    references = {
        m[1].strip().casefold(): m[2]
        for m in re.finditer(r"(?m)^\s{0,3}\[([^]\n]+)\]:\s*<?([^\s>]+)>?", text)
    }
    inline = r'!?\[[^]\n]*\]\(\s*(<[^>]+>|[^\s()]*(?:\([^()]*\)[^\s()]*)*)\s*(?:"[^"\n]*")?\)'
    for match in re.finditer(inline, text):
        yield text.count("\n", 0, match.start()) + 1, match[1].strip("<>")
    for match in re.finditer(r"!?\[([^]\n]+)\]\[([^]\n]*)\]", text):
        key = (match[2] or match[1]).strip().casefold()
        yield text.count("\n", 0, match.start()) + 1, references.get(key, "!undefined:" + key)
    for match in re.finditer(r"(?m)^\s{0,3}\[[^]\n]+\]:\s*<?([^\s>]+)>?", text):
        yield text.count("\n", 0, match.start()) + 1, match[1]
    for match in re.finditer(r'<(?:img|a|source)\b[^>]*?\b(?:src|href)=[\'\"]([^\'\"]+)', text, re.I):
        yield text.count("\n", 0, match.start()) + 1, match[1]


def main():
    # Include new, untracked docs so the pre-commit invocation validates them too.
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
    ).decode("utf-8").split("\0")
    files = sorted({ROOT / name for name in names if name.endswith(".md")
                    and not name.startswith(("components/", "third_party/", "licenses/"))
                    and (ROOT / name).is_file()})
    texts = {p.resolve(): p.read_text(encoding="utf-8") for p in files}
    graph = {p: set() for p in texts}
    errors = []
    checked = 0
    for p, text in texts.items():
        for line, target in links(text):
            if target.startswith("!undefined:"):
                errors.append(f"{p.relative_to(ROOT)}:{line}: {target}")
                continue
            url = urlsplit(html.unescape(target))
            if url.scheme or url.netloc:
                continue
            checked += 1
            path = unquote(url.path)
            q = ((ROOT if path.startswith("/") else p.parent) / path.lstrip("/")).resolve() if path else p
            if not q.exists():
                errors.append(f"{p.relative_to(ROOT)}:{line}: missing {target}")
                continue
            if q.is_dir():
                # GitHub's directory view exposes its Markdown files.
                graph[p].update(x.resolve() for x in q.glob("*.md"))
            else:
                graph[p].add(q)
            if url.fragment and q in texts and unquote(url.fragment) not in headings(texts[q]):
                errors.append(f"{p.relative_to(ROOT)}:{line}: missing heading {target}")
    pending = [(ROOT / "docs/README.md").resolve()]
    reached = set()
    while pending:
        p = pending.pop()
        if p not in reached:
            reached.add(p)
            pending.extend(graph.get(p, set()) - reached)
    docs = {p for p in texts if p.is_relative_to(ROOT / "docs")}
    for p in sorted(docs - reached):
        errors.append(f"{p.relative_to(ROOT)}: unreachable from docs/README.md")
    for error in errors:
        print(error)
    print(f"Checked {len(files)} Markdown files, {checked} local links, "
          f"{len(docs & reached)}/{len(docs)} docs reachable; {len(errors)} error(s).")
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
