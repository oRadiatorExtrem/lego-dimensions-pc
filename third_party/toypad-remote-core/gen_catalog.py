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


# Ids que o jogo NAO aceita (nao entram no catalogo, para a lista nao oferecer o que nao carrega).
#   769 Supergirl Red Lantern: exclusiva de outra plataforma; o jogo nao a carrega (TODO 0c do
#   ToyPad-Remote). O Cemu conhece 768-770 como especiais, mas 769 nao funciona no jogo.
EXCLUDED_IDS = {769}


def parse(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    marker = "window.CATALOG="
    start = text.index(marker) + len(marker)
    body = text[start:]
    # o proprio gerador acrescenta linhas ao fim do catalog.js (GADGETS/EXCLUDED_IDS): corta-las
    # antes de descodificar o JSON, senao a segunda execucao falha.
    for tail in ("window.GADGETS=", "window.EXCLUDED_IDS="):
        cut = body.find(tail)
        if cut >= 0:
            body = body[:cut]
    data = json.loads(body.strip().rstrip(";"))
    worlds = data["worlds"]
    entries = []
    for kind, rows in (("char", data.get("chars", [])), ("vehicle", data.get("vehicles", []))):
        for row in rows:
            ident = int(row[0])
            if ident in EXCLUDED_IDS:
                continue
            name = str(row[1])
            world = worlds[row[2]] if 0 <= int(row[2]) < len(worlds) else ""
            entries.append((ident, name, world, kind))
    entries.sort(key=lambda e: e[0])
    return entries


def parse_gadgets(path):
    """Ids dos gadgets (ver gadgets.tsv: fonte LEGO Dimensions Wiki)."""
    ids = []
    if not os.path.exists(path):
        return ids
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("	")
            if len(parts) < 2:
                parts = line.split()
            try:
                ids.append(int(parts[0], 0))
            except ValueError:
                continue
    return sorted(set(ids))


def c_string(text):
    out = text.replace("\\", "\\\\").replace('"', '\\"')
    return '"' + out + '"'


def emit(entries, out_path, gadgets=()):
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
        "// Gadgets (separados dos veiculos). Fonte: LEGO Dimensions Wiki, Category:Gadgets e as",
        "// subcategorias de ano; ver gadgets.tsv para os ids e o nome de cada um.",
        "inline constexpr uint16_t kGadgetIds[] = {%s};" % ", ".join("%du" % g for g in gadgets),
        "",
        "inline bool CatalogIsGadget(uint32_t id) {",
        "    for (uint16_t gadget : kGadgetIds) {",
        "        if (gadget == id) {",
        "            return true;",
        "        }",
        "    }",
        "    return false;",
        "}",
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
    gadgets = parse_gadgets(os.path.join(here, "gadgets.tsv"))
    emit(parse(src), dest, gadgets)
    # window.GADGETS para o web app: o chip "Gadgets" aparece quando esta lista existe
    if gadgets:
        marker = "window.CATALOG="
        text = open(src, encoding="utf-8").read()
        cut = text.find("window.GADGETS=")
        if cut >= 0:
            text = text[:cut].rstrip() + "\n"
        with open(src, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
            fh.write("window.GADGETS=[%s];\n" % ",".join(str(g) for g in gadgets))
        print("catalog.js: window.GADGETS com %d ids" % len(gadgets))
    # Os ids que o jogo nao aceita tambem tem de sair do lado do browser: o catalog.js e' a fonte
    # do catalogo no telemovel, e sem isto a figura continuava a ser oferecida na lista.
    if EXCLUDED_IDS:
        text = open(src, encoding="utf-8").read()
        cut = text.find("window.EXCLUDED_IDS=")
        if cut >= 0:
            text = text[:cut].rstrip() + "\n"
        with open(src, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
            fh.write("window.EXCLUDED_IDS=[%s];\n" % ",".join(str(i) for i in sorted(EXCLUDED_IDS)))
        print("catalog.js: window.EXCLUDED_IDS com %d ids" % len(EXCLUDED_IDS))


if __name__ == "__main__":
    main()
