#!/usr/bin/env python3
"""Gera catalog_data.h a partir de web_src/catalog.js.

O catalogo (id -> nome, mundo) vem do LD-ToyPad-Emulator (Berny23 e contribuidores, MIT) e
serve para dois efeitos: o nome canonico de uma figura (o utilizador pode renomear o ficheiro,
o id nao muda) e o mundo a que pertence. Nao leva imagens: os icones ficam como ficheiros
opcionais do utilizador (ver README).

Uso: python gen_catalog.py [catalog.js] [saida]
"""

import json
import os
import sys


def parse(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    marker = "window.CATALOG="
    start = text.index(marker) + len(marker)
    data = json.loads(text[start:].strip().rstrip(";"))
    worlds = data["worlds"]
    entries = []
    for kind, rows in (("char", data.get("chars", [])), ("vehicle", data.get("vehicles", []))):
        for row in rows:
            ident = int(row[0])
            name = str(row[1])
            world = worlds[row[2]] if 0 <= int(row[2]) < len(worlds) else ""
            entries.append((ident, name, world, kind))
    entries.sort(key=lambda e: e[0])
    return entries


def c_string(text):
    out = text.replace("\\", "\\\\").replace('"', '\\"')
    return '"' + out + '"'


def emit(entries, out_path):
    lines = [
        "// GERADO por gen_catalog.py a partir de web_src/catalog.js -- nao editar a mao.",
        "//",
        "// Nome e mundo de cada figura, por id. Dados do LD-ToyPad-Emulator (Berny23 e",
        "// contribuidores, licenca MIT). Nao contem imagens nem ficheiros de figuras.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace toypad_remote {",
        "",
        "struct CatalogEntry {",
        "    uint16_t id;",
        "    const char* name;",
        "    const char* world;",
        "    bool vehicle;",
        "};",
        "",
        "inline constexpr CatalogEntry kCatalog[] = {",
    ]
    for ident, name, world, kind in entries:
        lines.append("    {%du, %s, %s, %s}," % (ident, c_string(name), c_string(world), "true" if kind == "vehicle" else "false"))
    lines += [
        "};",
        "",
        "// Entrada do catalogo para `id` (nullptr se desconhecido). Tabela ordenada: procura binaria.",
        "inline const CatalogEntry* CatalogLookup(uint32_t id) {",
        "    if (id > 0xFFFFu) {",
        "        return nullptr;",
        "    }",
        "    size_t low = 0;",
        "    size_t high = sizeof(kCatalog) / sizeof(kCatalog[0]);",
        "    while (low < high) {",
        "        const size_t mid = (low + high) / 2;",
        "        if (kCatalog[mid].id < id) {",
        "            low = mid + 1;",
        "        } else {",
        "            high = mid;",
        "        }",
        "    }",
        "    return (low < sizeof(kCatalog) / sizeof(kCatalog[0]) && kCatalog[low].id == id) ? &kCatalog[low] : nullptr;",
        "}",
        "",
        "// Nome canonico (\"\" se desconhecido).",
        "inline const char* CatalogName(uint32_t id) {",
        "    const CatalogEntry* entry = CatalogLookup(id);",
        "    return entry ? entry->name : \"\";",
        "}",
        "",
        "// Mundo a que a figura pertence (\"\" se desconhecido).",
        "inline const char* CatalogWorld(uint32_t id) {",
        "    const CatalogEntry* entry = CatalogLookup(id);",
        "    return entry ? entry->world : \"\";",
        "}",
        "",
        "} // namespace toypad_remote",
        "",
    ]
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("%s: %d entradas" % (out_path, len(entries)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "web_src", "catalog.js")
    dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "catalog_data.h")
    emit(parse(src), dest)


if __name__ == "__main__":
    main()
