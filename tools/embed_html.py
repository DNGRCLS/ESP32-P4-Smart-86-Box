#!/usr/bin/env python3
"""Turn src/app/web/index.html into src/app/web/index_html.h (a C string the web server sends).
Run it after editing index.html:   python3 tools/embed_html.py"""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "src", "app", "web", "index.html")
dst = os.path.join(root, "src", "app", "web", "index_html.h")
text = open(src, encoding="utf-8").read()
out = ["/* Generated from index.html by tools/embed_html.py - edit index.html, then re-run the script. */",
       "#pragma once", "static const char index_html[] ="]
for line in text.split("\n"):
    out.append('"' + line.replace("\\", "\\\\").replace('"', '\\"').replace("??", "?\\?") + '\\n"')
out.append(";")
open(dst, "w", encoding="utf-8").write("\n".join(out) + "\n")
print("wrote", dst, len(text), "bytes")
