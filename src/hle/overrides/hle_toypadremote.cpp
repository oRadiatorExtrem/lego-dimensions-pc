// Adaptador do nucleo partilhado do Toy Pad Remote (third_party/toypad-remote-core) para o port.
//
// O mesmo nucleo corre dentro do fork de Cemu; aqui so se ligam os tres pontos de entrada:
//   * cada comando de 32 bytes que o jogo envia ao portal  -> Core::OnGameCommand
//   * cada mudanca no portal (por qualquer via)             -> Core::NotifyPortalChanged
//   * as accoes do telemovel                                -> hle::toypad (LoadTag, MoveIndex, ...)
//
// Regra de locks: o nucleo nunca e chamado com o g_mutex do hle::toypad na mao. Os callbacks sao
// disparados pelo DeferredNotify do hle_toypad, ja fora do lock (ver hle_toypad.cpp).

#include "hle_toypadremote.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "hle/overrides/hle_toypad.h"

#include "ToypadRemoteCore.h" // nucleo partilhado
#include "catalog_data.h"    // tabela de nomes partilhada


#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <fstream>
#include <string>
#include <vector>

namespace hle {

void InjectToypadPacket(const toypad::Packet& packet); // hle_usbd.cpp
void TakeToypadExternalControl();                      // hle_usbd.cpp

namespace toypad_remote {
namespace {

bool g_started = false;

// path -> std::string: o port compila em C++20 (u8string devolve u8string) e o mesmo codigo tem de
// funcionar onde u8string devolve std::string.
std::string NarrowPath(const std::filesystem::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return p.u8string();
#endif
}

// Converte o estado do port para o formato do nucleo. Copias simples, sem locks a sair daqui.
class RecompHost final : public ::toypad_remote::ToypadHost {
  public:
    explicit RecompHost(std::filesystem::path figures_dir) : m_figures(std::move(figures_dir)) {}

    ::toypad_remote::PortalSnapshot Snapshot() override {
        ::toypad_remote::PortalSnapshot snap;
        const auto slots = toypad::Slots(); // copia; o lock fica do lado do hle_toypad
        for (int i = 0; i < toypad::kSlotCount; ++i) {
            if (!slots[i].present) {
                continue;
            }
            snap.slots[static_cast<size_t>(i)].present = true;
            snap.slots[static_cast<size_t>(i)].id = slots[i].character_id;
            // o nucleo mostra o nome do ficheiro; o port guarda o caminho completo
            snap.slots[static_cast<size_t>(i)].file = FileNameOf(slots[i].path);
        }
        const auto leds = toypad::Leds();
        for (int zone = 0; zone < toypad::kPadCount; ++zone) {
            ::toypad_remote::LedState& led = snap.leds[static_cast<size_t>(zone)];
            switch (leds[zone].mode) {
            case 1:
                led.mode = 'c';
                break;
            case 2:
                led.mode = 'F';
                break;
            case 3:
                led.mode = 'f';
                break;
            default:
                led.mode = 'o';
                break;
            }
            led.r = leds[zone].r;
            led.g = leds[zone].g;
            led.b = leds[zone].b;
            led.p1 = leds[zone].mode == 2 ? leds[zone].on : leds[zone].speed;
            led.p2 = leds[zone].mode == 2 ? leds[zone].off : leds[zone].count;
            led.p3 = leds[zone].mode == 2 ? leds[zone].count : 0;
            led.seq = leds[zone].counter;
        }
        snap.session_ready = toypad::SessionReady();
        snap.awake = toypad::SessionReady();
        return snap;
    }

    bool LoadTag(int pad, int index, const ::toypad_remote::Tag& tag, const std::string& file, uint32_t* out_id,
                 std::string& err) override {
        toypad::TagData data{};
        std::copy(tag.begin(), tag.end(), data.begin());
        // se o ficheiro existir, e ele que manda e recebe as escritas (progresso do jogo)
        const std::filesystem::path path = m_figures / std::filesystem::u8path(file);
        std::error_code ec;
        const bool from_file = std::filesystem::exists(path, ec);
        u32 id = 0;
        const auto event = toypad::LoadTag(pad, index, data, from_file ? NarrowPath(path) : std::string(), &id);
        if (!event) {
            err = "could not load the figure";
            return false;
        }
        // o pacote de "figura colocada" TEM de chegar ao jogo; sem isto a figura aparece no telemovel
        // e no estado do portal, mas o jogo nunca a ve.
        Announce({*event});
        if (out_id) {
            *out_id = id;
        }
        return true;
    }

    bool RemoveIndex(int index, std::string& err) override {
        const auto event = toypad::RemoveIndex(index);
        if (!event) {
            err = "nothing in that slot";
            return false;
        }
        Announce({*event});
        return true;
    }

    bool MoveIndex(int pad, int index, int old_index, std::string& err) override {
        const auto events = toypad::MoveIndex(pad, index, old_index);
        if (events.empty()) {
            err = "nothing in that slot";
            return false;
        }
        Announce(events);
        return true;
    }

    bool Swap(int pad_a, int index_a, int pad_b, int index_b, std::string& err) override {
        const std::vector<toypad::Packet> events = toypad::SwapFigures(pad_a, index_a, pad_b, index_b);
        if (events.empty()) {
            err = "could not swap";
            return false;
        }
        // saida das duas, intervalo, entrada das duas trocadas (como num portal de verdade)
        std::vector<::toypad_remote::Packet> out;
        size_t removed = 0;
        for (const auto& packet : events) {
            const bool is_removal = packet.size() > 5 && packet[5] != 0; // byte 5 do evento: 1 = removida
            if (!is_removal) {
                break;
            }
            ++removed;
        }
        auto emit = [&](size_t from, size_t to) {
            std::vector<::toypad_remote::Packet> batch;
            for (size_t i = from; i < to && i < events.size(); ++i) {
                ::toypad_remote::Packet copy{};
                std::copy(events[i].begin(), events[i].end(), copy.begin());
                batch.push_back(copy);
            }
            Announce(batch);
        };
        emit(0, removed);
        if (removed < events.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            emit(removed, events.size());
        }
        return true;
    }

    bool CreateFigure(const std::string& path, uint32_t id, std::string& err) override {
        // personagem: tag sintetizada a partir de um UID e do id
        const toypad::TagData tag = toypad::BuildTag(RandomUid(), id);
        return WriteTagFile(path, tag, err);
    }

    bool CreateVehicle(const std::string& path, uint32_t id, std::string& err) override {
        const toypad::TagData tag = toypad::BuildTag(RandomUid(), id);
        return WriteTagFile(path, tag, err);
    }

    void Announce(const std::vector<::toypad_remote::Packet>& packets) override {
        if (packets.empty()) {
            return;
        }
        // a partir daqui o telemovel manda no portal (o portal.json deixa de valer), como o app
        // LegoToypad: as figuras que ele tinha posto saem na proxima passada da vigia
        TakeToypadExternalControl();
        for (const auto& packet : packets) {
            toypad::Packet copy{};
            std::copy(packet.begin(), packet.end(), copy.begin());
            InjectToypadPacket(copy);
        }
    }

    uint32_t IdentifyFigure(const ::toypad_remote::Tag& tag) override {
        toypad::TagData data{};
        std::copy(tag.begin(), tag.end(), data.begin());
        return toypad::TagCharacterId(data);
    }

    std::string CatalogName(uint32_t id) override {
        // tabela partilhada com o Cemu: id -> nome canonico (o ficheiro pode ser renomeado)
        return ::toypad_remote::CatalogName(id);
    }

    std::string FiguresDir() override {
        std::error_code ec;
        std::filesystem::create_directories(m_figures, ec);
        return NarrowPath(m_figures);
    }

    void Log(int level, const std::string& message) override {
        const char* tag = level >= 2 ? "TOYPAD-REMOTE ERROR" : "TOYPAD-REMOTE";
        std::printf("[%s] %s\n", tag, message.c_str());
        std::fflush(stdout);
    }

  private:
    static std::string FileNameOf(const std::string& path) {
        if (path.empty()) {
            return {};
        }
        return NarrowPath(std::filesystem::path(path).filename());
    }

    static toypad::Uid RandomUid() {
        // UID estavel por ficheiro nao e preciso: a tag fica gravada no .bin
        toypad::Uid uid{};
        static u32 state = 0x9E3779B9u;
        for (u8& byte : uid) {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<u8>(state >> 24);
        }
        return uid;
    }

    static bool WriteTagFile(const std::string& path, const toypad::TagData& tag, std::string& err) {
        std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "could not create the file";
            return false;
        }
        out.write(reinterpret_cast<const char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
        if (!out.good()) {
            err = "could not write the file";
            return false;
        }
        return true;
    }

    std::filesystem::path m_figures;
};

RecompHost* g_host = nullptr;

void OnGameCommand(const toypad::Packet& packet) {
    ::toypad_remote::Packet copy{};
    std::copy(packet.begin(), packet.end(), copy.begin());
    ::toypad_remote::Core::Instance().OnGameCommand(copy);
}

void OnPortalChanged() {
    ::toypad_remote::Core::Instance().NotifyPortalChanged();
}

} // namespace

bool Start(int port, const std::string& pin, const std::filesystem::path& figures_dir, std::string* err,
           const std::filesystem::path& web_dir) {
    if (g_started) {
        return true;
    }
    if (port < 0 || port > 65535) {
        if (err) {
            *err = "porta invalida";
        }
        return false;
    }
    if (!g_host) {
        g_host = new RecompHost(figures_dir.empty() ? std::filesystem::path("assets") / "toypad_remote" / "figures" : figures_dir);
    }
    ::toypad_remote::Config config;
    config.port = port;
    config.pin = pin;
    // icones (/img/<id>.png) e templates do utilizador ficam ao lado das figuras: nada disto e'
    // distribuido com o port. Sem a pasta, o web app mostra o marcador com o id.
    config.data_dir = figures_dir.parent_path().string();
    // pasta do site em disco (desenvolvimento): ganha ao site embutido, para se poder corrigir o
    // web app sem recompilar o jogo. Vazia = usa o embutido.
    if (!web_dir.empty()) {
        config.web_dir = NarrowPath(web_dir);
    }
    std::string reason;
    if (!::toypad_remote::Core::Instance().Start(g_host, config, reason)) {
        if (err) {
            *err = reason;
        }
        return false;
    }
    // o nucleo ja esta a correr: a partir daqui observa os comandos do jogo e as mudancas do portal
    toypad::SetCommandObserver(&OnGameCommand);
    toypad::SetPortalChangeCallback(&OnPortalChanged);
    g_started = true;
    return true;
}

void Stop() {
    if (!g_started) {
        return;
    }
    toypad::SetCommandObserver(nullptr);
    toypad::SetPortalChangeCallback(nullptr);
    ::toypad_remote::Core::Instance().Stop();
    g_started = false;
}

bool Running() {
    return g_started && ::toypad_remote::Core::Instance().Running();
}

std::string Url() {
    return g_started ? ::toypad_remote::Core::Instance().Url() : std::string();
}

} // namespace toypad_remote
} // namespace hle
