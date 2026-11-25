#!/usr/bin/env python3
"""Gera vehicle_upgrades.h a partir de vehicle_upgrades.tsv.

O TSV e' a fonte editavel (uma linha por id); o header e' gerado na build. Assim a
comunidade pode corrigir um valor sem recompilar nada a mao, e o nucleo continua a
distribuir apenas numeros: nenhum ficheiro de figura de terceiros entra no repositorio.

Uso: python gen_vehicle_upgrades.py [tsv] [saida]
"""

import os
import sys


def parse(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                parts = line.split()
            if len(parts) < 3:
                continue
            ident, p35, p37 = (int(x, 0) for x in parts[:3])
            rows.append((ident, p35, p37))
    rows.sort()
    return rows


def emit(rows, out_path):
    out = []
    out.append("// GERADO por gen_vehicle_upgrades.py a partir de vehicle_upgrades.tsv -- nao editar a mao.")
    out.append("//")
    out.append("// Melhorias de fabrica por id de veiculo/gadget, nas paginas 35 e 37 da tag. Sem isto a tag")
    out.append("// gerada leva zeros e o jogo responde 'Please update to access the upgrades for this item'.")
    out.append("// So numeros: nenhum ficheiro de figura de terceiros e' distribuido.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace toypad_remote {")
    out.append("")
    out.append("struct VehicleUpgrade {")
    out.append("    uint32_t id;")
    out.append("    uint32_t page35;")
    out.append("    uint32_t page37;")
    out.append("};")
    out.append("")
    out.append("inline constexpr VehicleUpgrade kVehicleUpgrades[] = {")
    for ident, p35, p37 in rows:
        out.append("    {0x%04Xu, 0x%08Xu, 0x%08Xu}," % (ident, p35, p37))
    out.append("};")
    out.append("")
    out.append("// Valores de fabrica do veiculo `id`. Devolve false se o id nao estiver na tabela.")
    out.append("inline bool FactoryUpgrades(uint32_t id, uint32_t* page35, uint32_t* page37) {")
    out.append("    for (const VehicleUpgrade& entry : kVehicleUpgrades) {")
    out.append("        if (entry.id == id) {")
    out.append("            if (page35) *page35 = entry.page35;")
    out.append("            if (page37) *page37 = entry.page37;")
    out.append("            return true;")
    out.append("        }")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")
    out.append("} // namespace toypad_remote")
    out.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out))
    print("%s: %d veiculos" % (out_path, len(rows)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    tsv = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "vehicle_upgrades.tsv")
    dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "vehicle_upgrades.h")
    emit(parse(tsv), dest)


if __name__ == "__main__":
    main()
