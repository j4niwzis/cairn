# Writes test/parser_cases.inc from ircdocs/parser-tests msg-split.yaml: a
# small reader for that file's shape (input, atoms: tags, source, verb, params).
import json, re, sys
src, out = sys.argv[1], sys.argv[2]
def value(v):
    v = v.strip()
    if v.startswith('"'): return yaml_double(v[1:-1])
    if v.startswith("'"): return v[1:-1].replace("''", "'")
    return v
def yaml_double(t):
    out, i = [], 0
    simple = {"\\": "\\", '"': '"', "n": "\n", "t": "\t", "r": "\r", "0": "\0", "e": "\x1b", "a": "\x07", "/": "/", " ": " "}
    while i < len(t):
        ch = t[i]
        if ch != "\\": out.append(ch); i += 1; continue
        n = t[i + 1]
        if n in simple: out.append(simple[n]); i += 2
        elif n == "x": out.append(chr(int(t[i+2:i+4], 16))); i += 4
        elif n == "u": out.append(chr(int(t[i+2:i+6], 16))); i += 6
        elif n == "U": out.append(chr(int(t[i+2:i+10], 16))); i += 10
        else: out.append(n); i += 2
    return "".join(out)
cases, cur, section = [], None, None
for raw in open(src, encoding="utf-8"):
    line = raw.rstrip("\n")
    if not line.strip() or line.strip().startswith("#"): continue
    m = re.match(r"^  - input: (.*)$", line)
    if m:
        cur = {"input": value(m.group(1)), "tags": None, "source": None, "verb": None, "params": []}
        cases.append(cur); section = None; continue
    if cur is None: continue
    s = line.strip()
    if s == "atoms:": continue
    if s == "tags:": cur["tags"] = []; section = "tags"; continue
    if s == "params:": section = "params"; continue
    if s.startswith("- ") and section == "params": cur["params"].append(value(s[2:])); continue
    km = re.match(r"^(\S+?|\"[^\"]*\"): (.*)$", s)
    if km:
        k, v = km.group(1), km.group(2)
        if section == "tags" and line.startswith("        "):
            cur["tags"].append((value(k), value(v) if v.strip() not in ("", "~", "null") else "")); continue
        section = None
        if k == "source": cur["source"] = value(v)
        elif k == "verb": cur["verb"] = value(v)
def c(s):
    out = '"'
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch == '"' or ch == "\\": out += "\\" + ch
        elif 32 <= b < 127: out += ch
        else: out += '\\x%02x" "' % b
    return out + '"' 
with open(out, "w", encoding="utf-8") as f:
    f.write("// Made by edits/gen_parser_tests.py from ircdocs/parser-tests (CC0): do not edit.\n")
    for k in cases:
        tags = "{" + ", ".join("{%s, %s}" % (c(a), c(b)) for a, b in (k["tags"] or [])) + "}"
        params = "{" + ", ".join(c(p) for p in k["params"]) + "}"
        f.write("{%s, %s, %s, %s, %s, %s},\n" % (c(k["input"]), "true" if k["tags"] is not None else "false", tags,
                c(k["source"]) if k["source"] is not None else "std::nullopt", c(k["verb"] or ""), params))
print(len(cases), "cases")
