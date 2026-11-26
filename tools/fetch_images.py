#!/usr/bin/env python3
"""Preenche os icones que faltam na pasta de imagens do Toy Pad Remote.

Fonte principal: harrysof/LegoToypad ("All Bin Files/<Mundo>/{Characters,Vehicules}/").
E' a mesma origem dos icones que o port ja usa, por isso o estilo (recorte, sem fundo) fica igual.
Alternativa: LEGO Dimensions Wiki (pageimages) para o que o repositorio nao tiver.

Nao mexe em nada existente: so escreve `<id>.png` para os ids que faltam.

Uso:
    python fetch_images.py --img <pasta_img> [--catalog <catalog.js>] [--dry-run]

Exemplos:
    python fetch_images.py --img "F:/.../assets/toypad_remote/img"
    python fetch_images.py --img ... --dry-run    # diz o que faria, sem descarregar

O relatorio final lista o que ficou por resolver; esses ids ficam no catalogo com as iniciais
do nome (o web app ja mostra isso), e podem ser acrescentados a OVERRIDES abaixo.
"""

import argparse
import difflib
import json
import os
import re
import sys
import unicodedata
import urllib.parse
import urllib.request

TREE_API = "https://api.github.com/repos/harrysof/LegoToypad/git/trees/HEAD?recursive=1"
RAW_BASE = "https://raw.githubusercontent.com/harrysof/LegoToypad/HEAD/"
WIKI_API = "https://lego-dimensions.fandom.com/api.php"
USER_AGENT = "toypad-remote-images/1.0"

# Nomes que o repositorio escreve de outra maneira. Chave = nome do catalogo (normalizado).
OVERRIDES = {
    "gandalf": "Gandalf The Grey",
    "the doctor": "Twelfth Doctor",
    "joker": "The Joker",
    "batblaster": "The Batblaster",
    "destroydozer": "The Destroydozer",
    "destroy dozer": "The Destroydozer",
    "bennys spaceship": "Benny Spaceship",
    "emmets excavator": "Emmet Excavator",
    "emmets car": "Emmet Car",
    "betelgeuse": "Beetle Juice",  # no repositorio e' "Beetle Juice.png"
}


def norm(text):
    text = unicodedata.normalize("NFD", text)
    text = "".join(c for c in text if unicodedata.category(c) != "Mn")
    text = text.lower().replace("'s", "").replace("&", "and")
    text = re.sub(r"\bthe\b", " ", text)  # "The Joker" == "Joker"
    return re.sub(r"[^a-z0-9]+", " ", text).strip()


def load_catalog(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    start = text.index("window.CATALOG=") + len("window.CATALOG=")
    body = text[start:]
    for tail in ("window.GADGETS=", "window.EXCLUDED_IDS="):
        cut = body.find(tail)
        if cut >= 0:
            body = body[:cut]
    data = json.loads(body.strip().rstrip(";"))
    excluded = set()
    m = re.search(r"window\.EXCLUDED_IDS=\[([^\]]*)\]", text)
    if m and m.group(1).strip():
        excluded = {int(x) for x in m.group(1).split(",")}
    names = {}
    for row in data["chars"] + data["vehicles"]:
        if row[0] not in excluded:
            names[row[0]] = row[1]
    return names


def fetch(url, binary=False):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read() if binary else response.read().decode("utf-8", "replace")


def repo_index():
    """nome normalizado -> URL raw do PNG no repositorio do harrysof."""
    tree = json.loads(fetch(TREE_API))
    if "tree" not in tree:
        raise SystemExit("GitHub respondeu sem arvore (limite de pedidos?): " + str(tree)[:200])
    index = {}
    for entry in tree["tree"]:
        path = entry["path"]
        if not path.lower().endswith(".png") or not path.startswith("All Bin Files/"):
            continue
        # "Logo/" tem imagens de mundo, nao da figura: so personagens e veiculos servem
        if "/Logo/" in path or "/Icons/" in path:
            continue
        base = path.split("/")[-1][:-4]
        # veiculos: "<Dono> - 1. <Nome>"; personagens: "<Nome>"
        match = re.match(r"^.*?-\s*\d+\.\s*(.+)$", base)
        index.setdefault(norm(match.group(1) if match else base), []).append(path)
    # um nome pode aparecer em varios sitios: preferir Characters/ e, dentro do mesmo, o primeiro
    for key, paths in index.items():
        paths.sort(key=lambda p: (0 if "/Characters/" in p else 1, p))
    return index


def wiki_image(name):
    """Thumbnail da wiki para o nome (fidelidade menor, mas cobre o que o repo nao tem)."""
    params = urllib.parse.urlencode(
        {
            "action": "query",
            "titles": name,
            "prop": "pageimages",
            "piprop": "original",
            "format": "json",
            "redirects": 1,
        }
    )
    try:
        data = json.loads(fetch(WIKI_API + "?" + params))
    except Exception:
        return None
    pages = data.get("query", {}).get("pages", {})
    for page in pages.values():
        original = page.get("original", {}).get("source")
        if original:
            return original
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--img", required=True, help="pasta das imagens (<id>.png)")
    parser.add_argument("--catalog", default=None, help="catalog.js (por omissao, ao lado do script)")
    parser.add_argument("--dry-run", action="store_true", help="so diz o que faria")
    args = parser.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    catalog = args.catalog or os.path.join(here, "..", "third_party", "toypad-remote-core", "web_src", "catalog.js")
    catalog = os.path.abspath(catalog)
    if not os.path.exists(catalog):
        sys.exit("catalogo nao encontrado: " + catalog)
    names = load_catalog(catalog)

    os.makedirs(args.img, exist_ok=True)
    have = {int(f[:-4]) for f in os.listdir(args.img) if f.lower().endswith(".png") and f[:-4].isdigit()}
    missing = sorted(i for i in names if i not in have)
    print("catalogo: %d ids | ja tens: %d | em falta: %d" % (len(names), len(have), len(missing)))
    if not missing:
        return

    unusable = {i for i in missing if "(unreleased)" in names[i].lower() or names[i].lower().startswith("world ")}
    if unusable:
        print("  %d sao 'unreleased'/placeholders: nao existe imagem em lado nenhum" % len(unusable))

    index = repo_index()
    got, failed = [], []
    for ident in missing:
        if ident in unusable:
            continue
        name = names[ident]
        key = norm(OVERRIDES.get(name.lower(), name))
        candidates = index.get(key)
        if not candidates:  # nomes que diferem por pouco ("Beetle Juice" vs "Betelgeuse")
            near = difflib.get_close_matches(key, index.keys(), n=1, cutoff=0.88)
            if near:
                candidates = index[near[0]]
                print("  ~      %5d %-32s parecido com %r" % (ident, name, near[0]))
        source = None
        if candidates:
            source = RAW_BASE + urllib.parse.quote(candidates[0])
        else:
            source = wiki_image(name)
        if not source:
            failed.append((ident, name, "sem correspondencia no repositorio nem na wiki"))
            print("  FALHA %5d %-32s" % (ident, name))
            continue
        if args.dry_run:
            print("  faria  %5d %-32s <- %s" % (ident, name, source))
            continue
        try:
            data = fetch(source, binary=True)
        except Exception as error:
            failed.append((ident, name, "download: %s" % error))
            print("  FALHA %5d %-32s (%s)" % (ident, name, error))
            continue
        if len(data) < 200:
            failed.append((ident, name, "ficheiro suspeito (%d bytes)" % len(data)))
            continue
        with open(os.path.join(args.img, "%d.png" % ident), "wb") as out:
            out.write(data)
        got.append(ident)
        print("  ok     %5d %-32s %d bytes" % (ident, name, len(data)))

    print("\ndescritos: %d | falharam: %d | unreleased: %d" % (len(got), len(failed), len(unusable)))
    if failed:
        report = os.path.join(args.img, "imagens_falhadas.txt")
        with open(report, "w", encoding="utf-8") as out:
            for ident, name, why in failed:
                out.write("%d\t%s\t%s\n" % (ident, name, why))
        print("relatorio: " + report)
        print("para resolveres: acrescenta o nome usado la' em OVERRIDES (fetch_images.py)")


if __name__ == "__main__":
    main()
