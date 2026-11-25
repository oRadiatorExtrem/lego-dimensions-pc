#!/usr/bin/env python3
"""Gera web_assets.h: o site (web_src/) embutido no executavel.

Uso:  python web_embed.py [pasta_web] [ficheiro_saida]

Sem argumentos usa web_src/ e web_assets.h ao lado deste script.
Nada de ficheiros de terceiros: so o que estiver em web_src/ (index.html, catalog.js,
manifest.json, icon.svg). As imagens (img/) e os templates (templates/) NAO entram no
repositorio; se existirem em disco sao servidas da pasta web_dir (ver Config).
"""

import io
import os
import sys

MIME = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".json": "application/json",
    ".webmanifest": "application/manifest+json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".css": "text/css",
    ".ico": "image/x-icon",
}


def collect(web_root):
    items = []
    for base, _dirs, files in os.walk(web_root):
        for name in sorted(files):
            full = os.path.join(base, name)
            rel = "/" + os.path.relpath(full, web_root).replace(os.sep, "/")
            if rel.startswith("/img/") or rel.startswith("/templates/"):
                continue  # conteudo de terceiros: nunca embutido
            ext = os.path.splitext(name)[1].lower()
            mime = MIME.get(ext)
            if not mime:
                continue
            with open(full, "rb") as fh:
                items.append((rel, mime, fh.read()))
    items.sort(key=lambda it: it[0])
    return items


def emit(items, out_path):
    out = io.StringIO()
    out.write("// GERADO por web_embed.py -- nao editar a mao.\n")
    out.write("// Site embutido no executavel (o que estiver em web_src/, sem img/ nem templates/).\n")
    out.write("#pragma once\n\nnamespace toypad_remote::embedded {\n\n")
    out.write("struct Asset {\n    const char* path;\n    const char* mime;\n    const unsigned char* data;\n")
    out.write("    unsigned int size;\n};\n\n")
    for index, (path, _mime, data) in enumerate(items):
        ident = "kData%d" % index
        out.write("inline const unsigned char %s[] = {" % ident)
        for chunk_start in range(0, len(data), 24):
            chunk = data[chunk_start : chunk_start + 24]
            out.write("\n    " + ",".join(str(b) for b in chunk) + ",")
        out.write("\n};\n")
    out.write("\ninline const Asset kAssets[] = {\n")
    for index, (path, mime, data) in enumerate(items):
        out.write('    {"%s", "%s", kData%d, %d},\n' % (path, mime, index, len(data)))
    out.write("};\n\ninline const unsigned int kAssetCount = %d;\n\n} // namespace toypad_remote::embedded\n" % len(items))
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(out.getvalue())
    total = sum(len(d) for _p, _m, d in items)
    print("%s: %d ficheiros, %d bytes" % (out_path, len(items), total))
    for path, _mime, data in items:
        print("  %-24s %7d" % (path, len(data)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    web_root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "web_src")
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "web_assets.h")
    if not os.path.isdir(web_root):
        sys.exit("pasta do site nao encontrada: " + web_root)
    emit(collect(web_root), out_path)


if __name__ == "__main__":
    main()
