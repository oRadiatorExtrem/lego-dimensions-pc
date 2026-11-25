// Atalhos do Toy Pad no comando, por jogador (F5 + F6).
//
// Sao os mesmos atalhos que o fork de Cemu tem (Home + botoes), adaptados ao comando do port:
// nao ha botao PS no XInput, por isso o modificador e' o TouchPad/Share, que o jogo quase nao usa
// (o Options fica livre: e' o que junta o jogador 2 no co-op).
//
// Tudo passa pelo nucleo partilhado (loadouts, zona que pisca, posicoes) e pelo hle::toypad
// (LoadTag / MoveIndex / RemoveIndex / SwapFigures), por isso o telemovel ve o resultado na hora:
// qualquer mudanca gera evento SSE.

#include "hle_toypadremote.h"

#include "hle/overrides/hle_toypad.h"

#include "ToypadRemoteCore.h" // nucleo partilhado
#include "catalog_data.h"    // nomes do catalogo (tabela partilhada)

#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

namespace hle::toypad_remote {
namespace {

// Bits do comando no formato do port (hle_pad.cpp): os mesmos numeros.
constexpr u32 kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u32 kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800;
constexpr u32 kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;
constexpr u32 kTouchPad = 0x100000;

constexpr int kPlayers = 4;
// Ordem em volta do portal: esquerda (0,3,4), centro (1), direita (2,5,6). Igual ao Cemu.
constexpr std::array<int, toypad::kSlotCount> kOrder = {0, 3, 4, 1, 2, 5, 6};
constexpr std::array<int, toypad::kSlotCount> kSlotPad = {2, 1, 3, 2, 2, 3, 3};
constexpr std::array<std::array<int, 3>, toypad::kPadCount> kPadSlots = {{{1, -1, -1}, {0, 3, 4}, {2, 5, 6}}};

struct PlayerState {
    u32 previous_buttons = 0;
    bool previous_modifier = false;
    int selected = -1; // posicao escolhida por ESTE jogador
};

std::array<PlayerState, kPlayers> g_players;

bool Started() {
    return ::toypad_remote::Core::Instance().Running();
}

void Toast(const std::string& text) {
    // Uma linha no log serve: o telemovel tem o estado completo e o jogo nao deve ser interrompido.
    std::printf("[TOYPAD-KEYS] %s\n", text.c_str());
    std::fflush(stdout);
}


void CycleSelection(PlayerState& state, int direction) {
    const auto slots = toypad::Slots();
    auto occupied = [&](int index) { return index >= 0 && index < toypad::kSlotCount && slots[index].present; };
    if (!occupied(state.selected)) {
        state.selected = -1;
    }
    int position = -1;
    for (int i = 0; i < toypad::kSlotCount; ++i) {
        if (kOrder[i] == state.selected) {
            position = i;
        }
    }
    for (int step = 1; step <= toypad::kSlotCount; ++step) {
        const int candidate = kOrder[static_cast<size_t>((position + direction * step + 7 * toypad::kSlotCount) % toypad::kSlotCount)];
        if (occupied(candidate)) {
            state.selected = candidate;
            return;
        }
    }
    state.selected = -1;
}

std::string SlotLabel(int slot) {
    const auto slots = toypad::Slots();
    if (slot < 0 || slot >= toypad::kSlotCount || !slots[slot].present) {
        return "(vazio)";
    }
    const std::string& path = slots[slot].path;
    if (!path.empty()) {
        return std::filesystem::path(path).stem().string();
    }
    const std::string name = ::toypad_remote::CatalogName(slots[slot].character_id);
    return name.empty() ? std::to_string(slots[slot].character_id) : name;
}

// Move a figura de `from` para a zona `pad`, trocando com o que la estiver se a zona estiver cheia.
// Igual ao que o telemovel faz no /api/zone.
int MoveToZone(int from, int pad) {
    auto slots = toypad::Slots();
    if (from < 0 || from >= toypad::kSlotCount || !slots[from].present || pad < 1 || pad > toypad::kPadCount) {
        return -1;
    }
    if (kSlotPad[static_cast<size_t>(from)] == pad) {
        return from;
    }
    for (int candidate : kPadSlots[static_cast<size_t>(pad - 1)]) {
        if (candidate >= 0 && !slots[candidate].present) {
            return toypad::MoveIndex(pad, candidate, from).empty() ? -1 : candidate;
        }
    }
    const int target = kPadSlots[static_cast<size_t>(pad - 1)][0];
    if (target < 0 || toypad::SwapFigures(pad, target, kSlotPad[static_cast<size_t>(from)], from).empty()) {
        return -1;
    }
    return target;
}

void ApplyLoadoutShortcut(int index) {
    const int count = ::toypad_remote::Core::Instance().LoadoutCount();
    if (index >= count) {
        Toast("loadout " + std::to_string(index + 1) + " nao existe (tem " + std::to_string(count) + ")");
        return;
    }
    std::string err;
    if (::toypad_remote::Core::Instance().ApplyLoadout(index, &err)) {
        Toast("loadout \"" + ::toypad_remote::Core::Instance().LoadoutName(index) + "\" aplicado");
    } else {
        Toast("loadout: " + err);
    }
}

void ZoneShortcut(int player, PlayerState& state, int pad, const char* what) {
    if (state.selected < 0) {
        Toast("nenhuma figura escolhida (segura TouchPad e carrega em Baixo)");
        return;
    }
    const int moved = MoveToZone(state.selected, pad);
    if (moved < 0) {
        Toast(std::string("nao consegui mover para ") + what);
        return;
    }
    state.selected = moved;
    Toast(std::string("para ") + what + ": " + SlotLabel(moved));
    ::toypad_remote::Core::Instance().SetSelectedForPlayer(player, moved);
}

void BlinkingShortcut(int player, PlayerState& state) {
    if (state.selected < 0) {
        Toast("nenhuma figura escolhida");
        return;
    }
    const int zone = ::toypad_remote::Core::Instance().BlinkingZone();
    if (zone == 0) {
        Toast("o jogo nao esta a piscar nenhuma zona");
        return;
    }
    static const char* const kNames[] = {"centro", "esquerda", "direita"};
    ZoneShortcut(player, state, zone, kNames[zone - 1]);
}

void HandlePress(int player, PlayerState& state, u32 pressed) {
    if (pressed & (kDown | kR1)) {
        CycleSelection(state, 1);
        Toast("escolhida: " + SlotLabel(state.selected));
        ::toypad_remote::Core::Instance().SetSelectedForPlayer(player, state.selected);
        return;
    }
    if (pressed & kL1) {
        CycleSelection(state, -1);
        Toast("escolhida: " + SlotLabel(state.selected));
        ::toypad_remote::Core::Instance().SetSelectedForPlayer(player, state.selected);
        return;
    }
    // zonas: esquerda, centro, direita (o mesmo desenho do portal no ecra)
    const int zone = (pressed & kLeft) ? 2 : (pressed & kUp) ? 1 : (pressed & kRight) ? 3 : 0;
    if (zone) {
        static const char* const kNames[] = {"centro", "esquerda", "direita"};
        ZoneShortcut(player, state, zone, kNames[zone - 1]);
        return;
    }
    if (pressed & kCross) {
        BlinkingShortcut(player, state); // TODO #1 do ToyPad-Remote: para a zona que o jogo esta a piscar
        return;
    }
    // loadouts 1..4
    const int loadout = (pressed & kSquare) ? 0 : (pressed & kCircle) ? 1 : (pressed & kTriangle) ? 2 : (pressed & kR2) ? 3 : -1;
    if (loadout >= 0) {
        ApplyLoadoutShortcut(loadout);
    }
}

} // namespace

u32 HandleGamepad(int player, u32 buttons) {
    if (!Started() || player < 0 || player >= kPlayers) {
        return buttons;
    }
    PlayerState& state = g_players[static_cast<size_t>(player)];
    const bool modifier = (buttons & kTouchPad) != 0;
    // Contam os botoes que aparecem AGORA e nao estavam no frame anterior, com o modificador em baixo.
    // Se o modificador vier no mesmo frame que o botao (comando com dois botoes de uma vez), conta
    // na mesma; e carregar no botao ANTES do modificador nao dispara nada, porque o botao ja estava
    // no estado anterior.
    const u32 pressed = modifier ? (buttons & ~state.previous_buttons) : 0;
    state.previous_buttons = buttons;
    state.previous_modifier = modifier;

    if (!modifier) {
        return buttons;
    }
    if (pressed) {
        HandlePress(player, state, pressed);
    }
    return 0; // com o modificador em baixo o jogo nao ve os botoes
}

bool GamepadConsumesButtons(int player, u32 buttons) {
    if (!Started() || player < 0 || player >= kPlayers) {
        return false;
    }
    (void)buttons;
    return (buttons & kTouchPad) != 0;
}

} // namespace hle::toypad_remote
