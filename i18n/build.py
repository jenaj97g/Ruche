#!/usr/bin/env python3
"""Builds the translation files from simple text sources.

  i18n/text/<code>.txt   one line per string: "<index><TAB><translation>" (\\n for a newline),
                         indexes as in i18n/strings.json (written by `build.py --extract`)
  -> i18n/ruche_<code>.ts  (Qt Linguist; also editable there) -> lrelease -> i18n/ruche_<code>.qm

  build.py --extract   re-reads the sources with lupdate into i18n/strings.json, keeping the
                       index of every string that still exists (new strings get new indexes)
  build.py             checks every text file (placeholders, HTML, file patterns) and writes
                       the .ts and .qm files; untranslated strings fall back to English

Serbian in Latin script (sr_Latn) is made from the Cyrillic text (sr) by transliteration.
"""
import json, os, re, subprocess, sys, tempfile
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUPDATE = os.environ.get("LUPDATE", "/usr/lib/qt6/bin/lupdate")
LRELEASE = os.environ.get("LRELEASE", "/usr/lib/qt6/bin/lrelease")
STRINGS = os.path.join(HERE, "strings.json")

# Qt's own strings (dialog buttons, the text-field menu), for languages Qt doesn't translate
QT_KEYS = [("QPlatformTheme", s) for s in ["OK", "Cancel", "Close", "Yes", "No", "Apply", "Save", "&Yes", "&No", "Open"]] + \
          [("QWidgetTextControl", s) for s in ["&Undo", "&Redo", "Cu&t", "&Copy", "&Paste", "Delete", "Select All"]] + \
          [("QLineEdit", s) for s in ["&Undo", "&Redo", "Cu&t", "&Copy", "&Paste", "Delete", "Select All"]]


def extract():
    old = json.load(open(STRINGS)) if os.path.exists(STRINGS) else []
    with tempfile.TemporaryDirectory() as tmp:
        ts = os.path.join(tmp, "x.ts")
        subprocess.run([LUPDATE, "-silent", "-no-obsolete", "-locations", "none", os.path.join(ROOT, "src"), "-ts", ts], check=True)
        found = []
        for c in ET.parse(ts).getroot().iter("context"):
            name = c.find("name").text
            for m in c.iter("message"):
                found.append((name, m.find("source").text))
    known = {(o["c"], o["s"]): o for o in old}
    out = [o for o in old if (o["c"], o["s"]) in set(found)]
    for i, o in enumerate(out):
        o["i"] = o.get("i", i)
    nxt = max([o["i"] for o in out], default=-1) + 1
    for c, s in found:
        if (c, s) not in known:
            out.append({"i": nxt, "c": c, "s": s})
            nxt += 1
    json.dump(out, open(STRINGS, "w"), ensure_ascii=False, indent=0)
    print(len(out), "strings,", sum(1 for c, s in found if (c, s) not in known), "new")


def placeholders(s):
    return sorted(re.findall(r"%\d", s))


def tags(s):
    return sorted(re.findall(r"</?[a-z0-9]+", s))


def patterns(s):
    return re.findall(r"\(\*[^)]*\)", s)


LAT = {"А": "A", "Б": "B", "В": "V", "Г": "G", "Д": "D", "Ђ": "Đ", "Е": "E", "Ж": "Ž", "З": "Z", "И": "I", "Ј": "J", "К": "K",
       "Л": "L", "Љ": "Lj", "М": "M", "Н": "N", "Њ": "Nj", "О": "O", "П": "P", "Р": "R", "С": "S", "Т": "T", "Ћ": "Ć", "У": "U",
       "Ф": "F", "Х": "H", "Ц": "C", "Ч": "Č", "Џ": "Dž", "Ш": "Š"}
LAT.update({k.lower(): v.lower() for k, v in LAT.items()})


def to_latin(s):
    # leave %1, tags, URLs and file patterns alone (they're ASCII anyway)
    return "".join(LAT.get(ch, ch) for ch in s)


def read_text(code):
    path = os.path.join(HERE, "text", code + ".txt")
    out, qt = {}, {}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        key, _, text = line.partition("\t")
        text = text.replace("\\n", "\n")
        if key.startswith("qt:"):
            qt[key[3:]] = text
        else:
            out[int(key)] = text
    return out, qt


def check(code, strings, tr):
    problems = []
    for o in strings:
        t = tr.get(o["i"])
        if t is None:
            problems.append("missing %d: %r" % (o["i"], o["s"]))
            continue
        if placeholders(t) != placeholders(o["s"]):
            problems.append("placeholders %d: %r -> %r" % (o["i"], o["s"], t))
        if tags(t) != tags(o["s"]):
            problems.append("html %d: %r -> %r" % (o["i"], o["s"], t))
        if patterns(t) != patterns(o["s"]):
            problems.append("file patterns %d" % o["i"])
    extra = set(tr) - {o["i"] for o in strings}
    if extra:
        problems.append("unknown indexes: %s" % sorted(extra))
    return problems


def write_ts(code, strings, tr, qt):
    lang = {"sr_Latn": "sr_Latn_RS"}.get(code, code)
    by_ctx = {}
    for o in strings:
        by_ctx.setdefault(o["c"], []).append((o["s"], tr.get(o["i"])))
    for (ctx, src) in QT_KEYS:
        key = src.replace("&", "")
        if key in qt:
            text = qt[key]
            if "&" in src and "&" not in text:
                text = "&" + text  # keep an accelerator
            by_ctx.setdefault(ctx, []).append((src, text))
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<!DOCTYPE TS>", '<TS version="2.1" language="%s" sourcelanguage="en">' % lang]
    for ctx, msgs in by_ctx.items():
        lines.append("<context>\n    <name>%s</name>" % escape(ctx))
        for src, text in msgs:
            lines.append("    <message>\n        <source>%s</source>" % escape(src))
            if text is None:
                lines.append('        <translation type="unfinished"></translation>')
            else:
                lines.append("        <translation>%s</translation>" % escape(text))
            lines.append("    </message>")
        lines.append("</context>")
    lines.append("</TS>")
    ts = os.path.join(HERE, "ruche_%s.ts" % code)
    open(ts, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return ts


def main():
    if "--extract" in sys.argv:
        extract()
        return
    strings = json.load(open(STRINGS))
    codes = sorted(f[:-4] for f in os.listdir(os.path.join(HERE, "text")) if f.endswith(".txt"))
    bad = False
    for code in codes:
        tr, qt = read_text(code)
        problems = check(code, strings, tr)
        done = sum(1 for o in strings if o["i"] in tr)
        print("%-6s %d/%d%s" % (code, done, len(strings), "" if not problems else "  (%d problems)" % len(problems)))
        for p in problems[:15]:
            print("       " + p)
        if any(not p.startswith("missing") for p in problems):
            bad = True
        outs = [(code, tr, qt)]
        if code == "sr":
            outs.append(("sr_Latn", {k: to_latin(v) for k, v in tr.items()}, {k: to_latin(v) for k, v in qt.items()}))
        for c, t, q in outs:
            ts = write_ts(c, strings, t, q)
            subprocess.run([LRELEASE, "-silent", ts, "-qm", ts[:-3] + ".qm"], check=True)
    # a template for languages still to be translated
    write_ts("template", strings, {}, {})
    # the resource file that puts them in the program
    qms = sorted(f for f in os.listdir(HERE) if f.startswith("ruche_") and f.endswith(".qm"))
    qts = sorted(f for f in os.listdir(os.path.join(HERE, "qt")) if f.endswith(".qm"))
    rows = ['<RCC>', '  <qresource prefix="/i18n">']
    rows += ['    <file>%s</file>' % f for f in qms]
    rows += ['    <file alias="%s">qt/%s</file>' % (f, f) for f in qts]
    rows += ['  </qresource>', '</RCC>']
    open(os.path.join(HERE, "i18n.qrc"), "w").write("\n".join(rows) + "\n")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
