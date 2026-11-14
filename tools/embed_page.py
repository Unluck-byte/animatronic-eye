# Embeds web/index.html into src/page.h before every build.
Import("env")
import os
proj = env["PROJECT_DIR"]
src = os.path.join(proj, "web", "index.html")
dst = os.path.join(proj, "src", "page.h")
html = open(src, encoding="utf-8").read()
out = ('#pragma once\n// Сгенерировано из web/index.html — правь там.\n'
       'const char PAGE[] PROGMEM = R"PAGE(' + html + ')PAGE";\n')
if not os.path.exists(dst) or open(dst, encoding="utf-8").read() != out:
    open(dst, "w", encoding="utf-8").write(out)
