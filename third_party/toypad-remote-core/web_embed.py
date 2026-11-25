#!/usr/bin/env python3
"""Gera web_assets.h: o site (web_src/) embutido no executavel.

Uso:  python web_embed.py [pasta_web] [ficheiro_saida]

Sem argumentos usa web_src/ e web_assets.h ao lado deste script.
Nada de ficheiros de terceiros: so o que estiver em web_src/ (index.html, catalog.js,
manifest.json, icon.svg). As imagens (img/) e os templates (templates/) NAO entram no
repositorio; se existirem em disco sao servidas da pasta data_dir (ver Config).

Cada ficheiro leva tambem uma variante comprimida (gzip), servida com
Content-Encoding: gzip quando o cliente a aceita: o index.html desce de 25 KB para ~6 KB,
o que se nota no telemovel em rede movel.
"""

import gzip
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


def emit_bytes(out, ident, data):
    out.write("inline const unsigned char %s[] = {" % ident)
    for start in range(0, len(data), 24):
        chunk = data[start : start + 24]
        out.write("\n    " + ",".join(str(b) for b in chunk) + ",")
    out.write("\n};\n")


def emit(items, out_path):
    out = io.StringIO()
    out.write("// GERADO por web_embed.py -- nao editar a mao.\n")
    out.write("// Site embutido no executavel (o que estiver em web_src/, sem img/ nem templates/).\n")
    out.write("#pragma once\n\nnamespace toypad_remote::embedded {\n\n")
    out.write("struct Asset {\n    const char* path;\n    const char* mime;\n    const unsigned char* data;\n")
    out.write("    unsigned int size;\n    // Variante comprimida (nullptr quando nao compensa): servida com Content-Encoding: gzip.\n")
    out.write("    const unsigned char* gz;\n    unsigned int gz_size;\n};\n\n")

    packed_info = []
    for index, (path, _mime, data) in enumerate(items):
        emit_bytes(out, "kData%d" % index, data)
        packed = gzip.compress(data, 9, mtime=0)
        if len(packed) < len(data):
            emit_bytes(out, "kGz%d" % index, packed)
            packed_info.append(("kGz%d" % index, len(packed)))
        else:
            packed_info.append((None, 0))

    out.write("\ninline const Asset kAssets[] = {\n")
    for index, (path, mime, data) in enumerate(items):
        gz_ident, gz_size = packed_info[index]
        if gz_ident:
            out.write('    {"%s", "%s", kData%d, %d, %s, %d},\n' % (path, mime, index, len(data), gz_ident, gz_size))
        else:
            out.write('    {"%s", "%s", kData%d, %d, nullptr, 0},\n' % (path, mime, index, len(data)))
    out.write("};\n\n")
    out.write("inline const unsigned int kAssetCount = %d;\n\n} // namespace toypad_remote::embedded\n" % len(items))

    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(out.getvalue())

    total = sum(len(d) for _p, _m, d in items)
    total_gz = sum(size for _i, size in packed_info)
    print("%s: %d ficheiros, %d bytes (%d comprimidos)" % (out_path, len(items), total, total_gz))
    for index, (path, _mime, data) in enumerate(items):
        gz_ident, gz_size = packed_info[index]
        print("  %-24s %7d%s" % (path, len(data), ("  -> %d gz" % gz_size) if gz_ident else "  (gz nao compensa)"))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    web_root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "web_src")
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "web_assets.h")
    if not os.path.isdir(web_root):
        sys.exit("pasta do site nao encontrada: " + web_root)
    emit(collect(web_root), out_path)


if __name__ == "__main__":
    main()
