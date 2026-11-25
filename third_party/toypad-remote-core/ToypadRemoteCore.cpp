// ToypadRemoteCore — implementacao. Ver ToypadRemoteCore.h para o contrato.
//
// Regras que este ficheiro garante:
//   * nenhum lock do anfitriao e pedido com o lock do nucleo na mao (e vice-versa): o estado e
//     copiado para valores simples e so depois formatado;
//   * um unico ponto de notificacao (NotifyPortalChanged) alimenta o SSE;
//   * todos os POST exigem X-TPR + Origin valido, OPTIONS e sempre recusado.

#include "ToypadRemoteCore.h"

#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "web_assets.h" // gerado por web_embed.py (site embutido no executavel)

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace fs = std::filesystem;

namespace toypad_remote {
namespace {

// Intervalo minimo entre dois eventos SSE do mesmo cliente. O jogo manda comandos de LED em rajada
// (animacao de zonas); sem isto cada cliente recebia dezenas de JSON por segundo.
constexpr auto kSseMinInterval = std::chrono::milliseconds(50);
constexpr auto kSseKeepAlive = std::chrono::seconds(10);
constexpr auto kPickupDelay = std::chrono::milliseconds(500);
constexpr const char* kHeaderToken = "X-TPR";
constexpr const char* kHeaderPin = "X-TPR-Pin";

// path -> std::string, seja o projeto C++17 (u8string devolve std::string) ou C++20
// (u8string devolve std::u8string). Sem isto o mesmo codigo nao compila nos dois anfitrioes.
std::string NarrowPath(const fs::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return p.u8string();
#endif
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// So nomes simples: "<nome>.bin", sem caminhos, sem "..", sem caracteres proibidos no Windows.
bool ValidFigureName(const std::string& name) {
    if (name.empty() || name.size() > 200) {
        return false;
    }
    if (name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos) {
        return false;
    }
    return EndsWith(name, ".bin");
}

std::string SanitizeName(std::string_view in) {
    std::string out;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '(' || c == ')' || c >= 0x80) {
            out += static_cast<char>(c);
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.size() > 80) {
        out.resize(80);
    }
    return out;
}

std::string HostOfOrigin(const std::string& origin) {
    // "http://192.168.1.20:8090" -> "192.168.1.20"; "http://[::1]:8090" -> "::1"
    const size_t scheme = origin.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    std::string rest = origin.substr(scheme + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        rest.resize(slash);
    }
    if (!rest.empty() && rest.front() == '[') {
        const size_t close = rest.find(']');
        return close == std::string::npos ? std::string() : rest.substr(1, close - 1);
    }
    const size_t colon = rest.find(':');
    return colon == std::string::npos ? rest : rest.substr(0, colon);
}

std::string HostOfHeader(const std::string& host_header) {
    if (!host_header.empty() && host_header.front() == '[') {
        const size_t close = host_header.find(']');
        return close == std::string::npos ? host_header : host_header.substr(1, close - 1);
    }
    const size_t colon = host_header.find(':');
    return colon == std::string::npos ? host_header : host_header.substr(0, colon);
}

bool IsLoopbackName(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "0.0.0.0";
}

#ifdef _WIN32
// Primeiro IPv4 de um adaptador ligado, que nao seja loopback nem APIPA. Enderecos Tailscale
// (100.64.0.0/10) sao preferidos no fim, como sufixo do URL anunciado.
std::string LanAddress(bool tailscale) {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) {
        return {};
    }
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const uint32_t ip = ntohl(in->sin_addr.s_addr);
            const bool apipa = (ip >> 16) == 0xA9FE; // 169.254.x.x
            if (apipa || (ip >> 24) == 127) {
                continue;
            }
            const bool is_tailscale = (ip >> 22) == (0x64400000 >> 22); // 100.64.0.0/10
            if (is_tailscale != tailscale) {
                continue;
            }
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
            return text;
        }
    }
    return {};
}
#else
std::string LanAddress(bool) { return {}; }
#endif

} // namespace

// -----------------------------------------------------------------------------------------------

struct Core::Impl {
    ToypadHost* host = nullptr;
    Config config;
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    std::atomic<bool> running{false};
    std::string url;
    int bound_port = 0;
    std::string boot_id;

    // Estado do servidor (alimentado pelos comandos do jogo). Ordem de locks: este `mutex` e o
    // primeiro; o do anfitriao vem depois e nunca os dois ao mesmo tempo (ver Snapshot).
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t version = 1;
    std::array<LedState, kPadCount> leds{};
    bool awake = false;
    int selected = -1;

    // ---------------------------------------------------------------- estado
    void Bump() { // caller holds mutex
        ++version;
        cv.notify_all();
    }

    std::string StateJson() {
        // 1) estado do servidor: copiar e largar o lock
        std::array<LedState, kPadCount> leds_copy;
        bool awake_copy = false;
        int selected_copy = -1;
        uint64_t version_copy = 0;
        {
            std::lock_guard lock(mutex);
            leds_copy = leds;
            awake_copy = awake;
            selected_copy = selected;
            version_copy = version;
        }
        // 2) estado do pad: o anfitriao tranca o que precisar e larga antes de voltar
        PortalSnapshot snap;
        if (host) {
            snap = host->Snapshot();
        }
        snap.awake = snap.awake || awake_copy;
        // 3) serializar, sem locks nenhuns na mao
        std::string j;
        j.reserve(512);
        j += "{\"boot\":\"";
        j += boot_id;
        j += "\",\"v\":";
        j += std::to_string(version_copy);
        j += ",\"awake\":";
        j += snap.awake ? "true" : "false";
        j += ",\"sel\":";
        j += std::to_string(selected_copy);
        j += ",\"slots\":[";
        for (int i = 0; i < kSlotCount; ++i) {
            if (i) {
                j += ',';
            }
            const SlotState& s = snap.slots[static_cast<size_t>(i)];
            if (!s.present) {
                j += "null";
            } else {
                j += "{\"id\":";
                j += std::to_string(s.id);
                j += ",\"file\":\"";
                j += JsonEscape(s.file);
                j += "\"}";
            }
        }
        j += "],\"leds\":[";
        for (int i = 0; i < kPadCount; ++i) {
            if (i) {
                j += ',';
            }
            const LedState& l = leds_copy[static_cast<size_t>(i)];
            char buf[160];
            std::snprintf(buf, sizeof(buf), "{\"m\":\"%c\",\"c\":[%u,%u,%u],\"p\":[%u,%u,%u],\"s\":%u}",
                          l.mode ? l.mode : 'o', l.r, l.g, l.b, l.p1, l.p2, l.p3, l.seq);
            j += buf;
        }
        j += "]}";
        return j;
    }

    // ---------------------------------------------------------------- LEDs
    void SetLed(uint8_t pad, char mode, uint8_t r, uint8_t g, uint8_t b, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0) {
        bool changed = false;
        {
            std::lock_guard lock(mutex);
            auto apply = [&](LedState& l) {
                if (mode == 'c' && l.mode == 'c' && l.r == r && l.g == g && l.b == b) {
                    return false;
                }
                l.mode = mode;
                l.r = r, l.g = g, l.b = b;
                l.p1 = p1, l.p2 = p2, l.p3 = p3;
                ++l.seq;
                return true;
            };
            if (pad == 0) {
                for (auto& l : leds) {
                    changed |= apply(l);
                }
            } else if (pad <= kPadCount) {
                changed = apply(leds[pad - 1]);
            }
            if (changed) {
                Bump();
            }
        }
    }

    void OnGameCommand(const Packet& command) {
        const uint8_t cmd = command[2];
        const uint8_t* p = command.data() + 4; // payload
        if (cmd < 0xB0 || cmd > 0xC8) {
            return;
        }
        switch (cmd) {
        case 0xB0: { // wake: o jogo (re)ligou ao pad
            std::lock_guard lock(mutex);
            awake = true;
            Bump();
            break;
        }
        case 0xC0: // cor: pad, r, g, b
            SetLed(p[0], 'c', p[1], p[2], p[3]);
            break;
        case 0xC2: // fade: pad, velocidade, ciclos, r, g, b
            SetLed(p[0], 'f', p[3], p[4], p[5], p[1], p[2]);
            break;
        case 0xC3: // flash: pad, aceso, apagado, ciclos, r, g, b
            SetLed(p[0], 'F', p[4], p[5], p[6], p[1], p[2], p[3]);
            break;
        case 0xC4: // fade aleatorio: pad, velocidade, ciclos
            SetLed(p[0], 'r', 0, 0, 0, p[1], p[2]);
            break;
        case 0xC6: // fade todos: por zona [ativo, velocidade, ciclos, r, g, b]
            for (uint8_t i = 0; i < kPadCount; ++i) {
                const uint8_t* q = p + i * 6;
                if (q[0]) {
                    SetLed(static_cast<uint8_t>(i + 1), 'f', q[3], q[4], q[5], q[1], q[2]);
                }
            }
            break;
        case 0xC7: // flash todos: por zona [ativo, aceso, apagado, ciclos, r, g, b]
            for (uint8_t i = 0; i < kPadCount; ++i) {
                const uint8_t* q = p + i * 7;
                if (q[0]) {
                    SetLed(static_cast<uint8_t>(i + 1), 'F', q[4], q[5], q[6], q[1], q[2], q[3]);
                }
            }
            break;
        case 0xC8: // cor todos: por zona [ativo, r, g, b]
            for (uint8_t i = 0; i < kPadCount; ++i) {
                const uint8_t* q = p + i * 4;
                if (q[0]) {
                    SetLed(static_cast<uint8_t>(i + 1), 'c', q[1], q[2], q[3]);
                }
            }
            break;
        case 0xC1: // desligar: pad (0 = todos)
            if (p[0] == 0) {
                for (uint8_t i = 0; i < kPadCount; ++i) {
                    SetLed(static_cast<uint8_t>(i + 1), 'o', 0, 0, 0);
                }
            } else {
                SetLed(p[0], 'o', 0, 0, 0);
            }
            break;
        default:
            break;
        }
    }

    // ---------------------------------------------------------------- ficheiros
    fs::path FiguresDir() const { return host ? fs::path(host->FiguresDir()) : fs::path(); }

    fs::path FigurePath(const std::string& name) const {
        if (!ValidFigureName(name)) {
            return {};
        }
        return FiguresDir() / fs::path(name);
    }

    bool ReadFigure(const fs::path& path, Tag& tag) const {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        in.read(reinterpret_cast<char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
        return in.gcount() == static_cast<std::streamsize>(tag.size());
    }

    std::set<std::string> ReadFavorites() const {
        std::set<std::string> favs;
        std::ifstream in(FiguresDir() / "favorites.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                favs.insert(line);
            }
        }
        return favs;
    }

    void WriteFavorites(const std::set<std::string>& favs) const {
        std::ofstream out(FiguresDir() / "favorites.txt", std::ios::trunc);
        for (const auto& f : favs) {
            out << f << '\n';
        }
    }

    struct Loadout {
        std::string name;
        std::array<std::string, kSlotCount> files;
    };

    std::vector<Loadout> ReadLoadouts() const {
        std::vector<Loadout> list;
        std::ifstream in(FiguresDir() / "loadouts.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            Loadout l;
            size_t start = 0;
            for (int field = 0; field <= kSlotCount; ++field) {
                const size_t tab = line.find('\t', start);
                const std::string value = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
                if (field == 0) {
                    l.name = value;
                } else {
                    l.files[static_cast<size_t>(field - 1)] = value;
                }
                if (tab == std::string::npos) {
                    break;
                }
                start = tab + 1;
            }
            list.push_back(std::move(l));
        }
        return list;
    }

    void WriteLoadouts(const std::vector<Loadout>& list) const {
        std::ofstream out(FiguresDir() / "loadouts.txt", std::ios::trunc);
        for (const auto& l : list) {
            out << l.name;
            for (const auto& f : l.files) {
                out << '\t' << f;
            }
            out << '\n';
        }
    }

    void RenameReferences(const std::string& from, const std::string& to) const {
        auto favs = ReadFavorites();
        if (favs.erase(from) && !to.empty()) {
            favs.insert(to);
        }
        WriteFavorites(favs);
        auto list = ReadLoadouts();
        for (auto& l : list) {
            for (auto& f : l.files) {
                if (f == from) {
                    f = to;
                }
            }
        }
        WriteLoadouts(list);
    }

    // ---------------------------------------------------------------- pad
    PortalSnapshot SnapshotNow() {
        if (!host) {
            return {};
        }
        return host->Snapshot();
    }

    bool IsOnPad(const PortalSnapshot& snap, const std::string& file) const {
        for (int i = 0; i < kSlotCount; ++i) {
            if (snap.slots[static_cast<size_t>(i)].present && snap.slots[static_cast<size_t>(i)].file == file) {
                return true;
            }
        }
        return false;
    }

    std::string SlotLabel(const PortalSnapshot& snap, int slot) {
        const SlotState& s = snap.slots[static_cast<size_t>(slot)];
        if (!s.present) {
            return "(vazio)";
        }
        if (!s.file.empty()) {
            return s.file.substr(0, s.file.size() - 4); // tira o ".bin"
        }
        if (host) {
            const std::string name = host->CatalogName(s.id);
            if (!name.empty()) {
                return name;
            }
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", s.id);
        return buf;
    }

    // Tira o que esta na posicao e volta a po-la depois do intervalo: e o que um portal de verdade faz.
    bool ReplaceSlot(int slot, const std::string& file, uint32_t* out_id, std::string& err) {
        Tag tag{};
        const fs::path path = FigurePath(file);
        if (path.empty()) {
            err = "invalid file name";
            return false;
        }
        if (!ReadFigure(path, tag)) {
            err = "figure file not found or not a 180 byte dump";
            return false;
        }
        PortalSnapshot snap = SnapshotNow();
        if (snap.slots[static_cast<size_t>(slot)].present) {
            if (!host->RemoveIndex(slot, err)) {
                return false;
            }
            std::this_thread::sleep_for(kPickupDelay);
        }
        if (!host->LoadTag(kPadOf(slot), slot, tag, file, out_id, err)) {
            return false;
        }
        return true;
    }

    static int kPadOf(int slot) { return kSlotPad[static_cast<size_t>(slot)]; }

    std::string PlaceFigure(int slot, const std::string& file, uint32_t* out_id) {
        if (!ValidFigureName(file)) {
            return "invalid file name";
        }
        const PortalSnapshot snap = SnapshotNow();
        // uma tag so pode estar num sitio: se ja esta no pad, muda de posicao
        for (int i = 0; i < kSlotCount; ++i) {
            const SlotState& s = snap.slots[static_cast<size_t>(i)];
            if (s.present && s.file == file) {
                if (i == slot) {
                    return {}; // ja esta onde se quer
                }
                std::string err;
                if (!host->MoveIndex(kPadOf(slot), slot, i, err)) {
                    return err.empty() ? "could not move the figure" : err;
                }
                return {};
            }
        }
        std::string err;
        if (!ReplaceSlot(slot, file, out_id, err)) {
            return err;
        }
        return {};
    }

    int MoveToZone(int from, int pad, std::string& err) {
        PortalSnapshot snap = SnapshotNow();
        if (from < 0 || from >= kSlotCount || !snap.slots[static_cast<size_t>(from)].present) {
            err = "no figure in that slot";
            return -1;
        }
        if (kPadOf(from) == pad) {
            return from;
        }
        for (uint8_t s : kPadSlots[static_cast<size_t>(pad - 1)]) {
            if (s != 255 && !snap.slots[s].present) {
                if (!host->MoveIndex(pad, s, from, err)) {
                    return -1;
                }
                return s;
            }
        }
        const int target = kPadSlots[static_cast<size_t>(pad - 1)][0];
        if (!host->Swap(pad, target, kPadOf(from), from, err)) {
            return -1;
        }
        return target;
    }

    std::string ApplyLoadout(const Loadout& loadout) {
        // tira o que nao faz parte, depois poe cada figura no seu sitio
        PortalSnapshot snap = SnapshotNow();
        std::string err;
        for (int i = 0; i < kSlotCount; ++i) {
            const SlotState& s = snap.slots[static_cast<size_t>(i)];
            if (!s.present) {
                continue;
            }
            const std::string& want = loadout.files[static_cast<size_t>(i)];
            if (want == s.file) {
                continue;
            }
            if (!host->RemoveIndex(i, err)) {
                return err;
            }
            std::this_thread::sleep_for(kPickupDelay);
        }
        for (int i = 0; i < kSlotCount; ++i) {
            const std::string& want = loadout.files[static_cast<size_t>(i)];
            if (want.empty()) {
                continue;
            }
            uint32_t id = 0;
            if (!ReplaceSlot(i, want, &id, err)) {
                return err;
            }
            std::this_thread::sleep_for(kPickupDelay);
        }
        NotifyPortalChanged();
        return {};
    }

    void NotifyPortalChanged() {
        {
            std::lock_guard lock(mutex);
            Bump();
        }
    }

    // ---------------------------------------------------------------- HTTP helpers
    void Fail(httplib::Response& res, int status, const std::string& message) const {
        res.status = status;
        res.set_content("{\"error\":\"" + JsonEscape(message) + "\"}", "application/json");
    }

    void Ok(httplib::Response& res) const { res.set_content("{\"ok\":true}", "application/json"); }

    static std::optional<int> SlotParam(const httplib::Request& req, const char* key) {
        if (!req.has_param(key)) {
            return std::nullopt;
        }
        const std::string v = req.get_param_value(key);
        if (v.size() != 1 || v[0] < '0' || v[0] > '0' + kSlotCount - 1) {
            return std::nullopt;
        }
        return v[0] - '0';
    }

    bool OriginAllowed(const httplib::Request& req, const std::string& origin) const {
        const std::string host = HostOfOrigin(origin);
        if (host.empty()) {
            return false;
        }
        if (IsLoopbackName(host)) {
            return true;
        }
        const std::string request_host = HostOfHeader(req.get_header_value("Host"));
        if (!request_host.empty() && host == request_host) {
            return true; // o proprio web app, servido por nos
        }
        // o URL anunciado (IP do adaptador de rede) tambem e aceite
        return !url.empty() && host == HostOfOrigin(url);
    }

    bool PinOk(const httplib::Request& req) const {
        if (config.pin.empty()) {
            return true;
        }
        if (req.get_header_value(kHeaderPin) == config.pin) {
            return true;
        }
        return req.has_param("pin") && req.get_param_value("pin") == config.pin;
    }

    // ---------------------------------------------------------------- estaticos
    bool ServeAsset(const httplib::Request& req, httplib::Response& res) const {
        std::string path = req.path;
        if (path == "/") {
            path = "/index.html";
        }
        // 1) pasta em disco (desenvolvimento) ganha ao embutido
        if (!config.web_dir.empty()) {
            std::error_code ec;
            const fs::path candidate = fs::path(config.web_dir) / fs::path(path.substr(1));
            if (fs::is_regular_file(candidate, ec)) {
                std::ifstream in(candidate, std::ios::binary);
                if (in) {
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    res.set_content(ss.str(), MimeFor(path));
                    std::string cache = EndsWith(path, ".html") ? "no-cache" : "max-age=86400";
                    res.set_header("Cache-Control", cache);
                    return true;
                }
            }
        }
        // 2) embutido
        for (unsigned int i = 0; i < embedded::kAssetCount; ++i) {
            if (path == embedded::kAssets[i].path) {
                res.set_content(reinterpret_cast<const char*>(embedded::kAssets[i].data), embedded::kAssets[i].size,
                                embedded::kAssets[i].mime);
                res.set_header("Cache-Control", EndsWith(path, ".html") ? "no-cache" : "max-age=86400");
                return true;
            }
        }
        return false;
    }

    static const char* MimeFor(const std::string& path) {
        if (EndsWith(path, ".html")) return "text/html";
        if (EndsWith(path, ".js")) return "text/javascript";
        if (EndsWith(path, ".json")) return "application/json";
        if (EndsWith(path, ".svg")) return "image/svg+xml";
        if (EndsWith(path, ".png")) return "image/png";
        if (EndsWith(path, ".css")) return "text/css";
        if (EndsWith(path, ".webmanifest")) return "application/manifest+json";
        return "application/octet-stream";
    }

    // ---------------------------------------------------------------- servidor
    void RegisterRoutes(httplib::Server& srv);
    bool Start(ToypadHost* h, const Config& cfg, std::string& err);
    void Stop();
};

void Core::Impl::RegisterRoutes(httplib::Server& srv) {
    // Seguranca: antes de qualquer rota.
    srv.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "OPTIONS") {
            // Sem preflight nao ha CORS: um site externo nao consegue enviar o cabecalho X-TPR.
            res.status = 403;
            res.set_content("{\"error\":\"no CORS\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !OriginAllowed(req, origin)) {
            res.status = 403;
            res.set_content("{\"error\":\"origin not allowed\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method == "POST") {
            if (req.get_header_value(kHeaderToken) != "1") {
                res.status = 403;
                res.set_content("{\"error\":\"missing X-TPR header\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!PinOk(req)) {
                res.status = 401;
                res.set_content("{\"error\":\"PIN required\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    srv.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(StateJson(), "application/json");
    });

    srv.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Accel-Buffering", "no");
        struct Client {
            uint64_t last_sent = 0;
            std::chrono::steady_clock::time_point next_allowed{};
        };
        auto client = std::make_shared<Client>();
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, client](size_t, httplib::DataSink& sink) {
                if (!sink.is_writable()) {
                    return false;
                }
                // coalescer: no maximo um evento por kSseMinInterval neste cliente
                const auto now = std::chrono::steady_clock::now();
                if (client->next_allowed > now) {
                    std::this_thread::sleep_for(client->next_allowed - now);
                }
                uint64_t version_now = 0;
                {
                    std::unique_lock lock(mutex);
                    cv.wait_for(lock, kSseKeepAlive, [&] { return version != client->last_sent; });
                    version_now = version;
                }
                std::string msg;
                if (version_now != client->last_sent) {
                    client->last_sent = version_now;
                    msg = "data: " + StateJson() + "\n\n"; // sem locks
                } else {
                    msg = ": ping\n\n"; // keep-alive para o telemovel notar ligacoes mortas
                }
                client->next_allowed = std::chrono::steady_clock::now() + kSseMinInterval;
                return sink.write(msg.data(), msg.size());
            });
    });

    srv.Get("/api/figures", [this](const httplib::Request&, httplib::Response& res) {
        const auto favs = ReadFavorites();
        PortalSnapshot snap = SnapshotNow();
        std::string j = "[";
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(FiguresDir(), ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin") {
                continue;
            }
            const std::string name = NarrowPath(entry.path().filename());
            Tag tag{};
            const bool ok = ReadFigure(entry.path(), tag);
            const uint32_t id = (ok && host) ? host->IdentifyFigure(tag) : 0;
            if (j.size() > 1) {
                j += ',';
            }
            j += "{\"file\":\"" + JsonEscape(name) + "\",\"id\":" + std::to_string(id) + ",\"fav\":" +
                 (favs.count(name) ? "true" : "false") + ",\"on\":" + (IsOnPad(snap, name) ? "true" : "false") + "}";
        }
        j += ']';
        res.set_header("Cache-Control", "no-store");
        res.set_content(j, "application/json");
    });

    srv.Post("/api/figures/fav", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string file = req.get_param_value("file");
        if (!ValidFigureName(file)) {
            return Fail(res, 400, "invalid file name");
        }
        auto favs = ReadFavorites();
        if (req.get_param_value("on") == "1") {
            favs.insert(file);
        } else {
            favs.erase(file);
        }
        WriteFavorites(favs);
        Ok(res);
    });

    srv.Post("/api/figures/rename", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string file = req.get_param_value("file");
        const std::string to = SanitizeName(req.get_param_value("to")) + ".bin";
        const fs::path from = FigurePath(file);
        const fs::path dest = FigurePath(to);
        if (from.empty() || dest.empty() || to == ".bin") {
            return Fail(res, 400, "invalid name");
        }
        if (IsOnPad(SnapshotNow(), file)) {
            return Fail(res, 400, "tira a figura do pad primeiro");
        }
        std::error_code ec;
        if (fs::exists(dest, ec)) {
            return Fail(res, 400, "ja existe uma figura com esse nome");
        }
        fs::rename(from, dest, ec);
        if (ec) {
            return Fail(res, 500, "could not rename");
        }
        RenameReferences(file, to);
        res.set_content("{\"ok\":true,\"file\":\"" + JsonEscape(to) + "\"}", "application/json");
    });

    srv.Post("/api/figures/delete", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string file = req.get_param_value("file");
        const fs::path from = FigurePath(file);
        if (from.empty()) {
            return Fail(res, 400, "invalid file name");
        }
        if (IsOnPad(SnapshotNow(), file)) {
            return Fail(res, 400, "tira a figura do pad primeiro");
        }
        std::error_code ec;
        const fs::path bin = FiguresDir() / "_apagadas";
        fs::create_directories(bin, ec);
        fs::path dest = bin / fs::path(file);
        for (int n = 2; fs::exists(dest, ec); ++n) {
            dest = bin / fs::path(file.substr(0, file.size() - 4) + " (" + std::to_string(n) + ").bin");
        }
        fs::rename(from, dest, ec);
        if (ec) {
            return Fail(res, 500, "could not delete");
        }
        RenameReferences(file, "");
        Ok(res);
    });

    srv.Post("/api/place", [this](const httplib::Request& req, httplib::Response& res) {
        const auto slot = SlotParam(req, "slot");
        if (!slot || !req.has_param("file")) {
            return Fail(res, 400, "slot (0-6) and file required");
        }
        uint32_t id = 0;
        const std::string err = PlaceFigure(*slot, req.get_param_value("file"), &id);
        NotifyPortalChanged();
        if (!err.empty()) {
            return Fail(res, 400, err);
        }
        Ok(res);
    });

    srv.Post("/api/remove", [this](const httplib::Request& req, httplib::Response& res) {
        const auto slot = SlotParam(req, "slot");
        if (!slot) {
            return Fail(res, 400, "slot (0-6) required");
        }
        std::string err;
        host->RemoveIndex(*slot, err);
        NotifyPortalChanged();
        Ok(res);
    });

    srv.Post("/api/zone", [this](const httplib::Request& req, httplib::Response& res) {
        const auto slot = SlotParam(req, "slot");
        const int pad = std::atoi(req.get_param_value("pad").c_str());
        if (!slot || pad < 1 || pad > kPadCount) {
            return Fail(res, 400, "slot (0-6) and pad (1-3) required");
        }
        std::string err;
        const int moved = MoveToZone(*slot, pad, err);
        NotifyPortalChanged();
        if (moved < 0) {
            return Fail(res, 400, err.empty() ? "no figure in that slot" : err);
        }
        Ok(res);
    });

    srv.Post("/api/move", [this](const httplib::Request& req, httplib::Response& res) {
        const auto from = SlotParam(req, "from");
        const auto to = SlotParam(req, "to");
        if (!from || !to) {
            return Fail(res, 400, "from and to (0-6) required");
        }
        PortalSnapshot snap = SnapshotNow();
        if (!snap.slots[static_cast<size_t>(*from)].present) {
            return Fail(res, 400, "no figure in that slot");
        }
        std::string err;
        bool ok = true;
        if (*from != *to && snap.slots[static_cast<size_t>(*to)].present) {
            // largar em cima de outra figura troca as duas em vez de a derrubar
            ok = host->Swap(kPadOf(*to), *to, kPadOf(*from), *from, err);
        } else if (*from != *to) {
            ok = host->MoveIndex(kPadOf(*to), *to, *from, err);
        }
        NotifyPortalChanged();
        if (!ok) {
            return Fail(res, 400, err.empty() ? "could not move" : err);
        }
        Ok(res);
    });

    srv.Post("/api/create", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("id")) {
            return Fail(res, 400, "id required");
        }
        const unsigned long raw = std::strtoul(req.get_param_value("id").c_str(), nullptr, 10);
        if (raw > 0xFFFF) {
            return Fail(res, 400, "id out of range");
        }
        const uint32_t id = static_cast<uint32_t>(raw);
        std::string base = SanitizeName(req.get_param_value("name"));
        if (base.empty()) {
            base = "Figure " + std::to_string(id);
        }
        std::string name = base + ".bin";
        std::error_code ec;
        for (int n = 2; fs::exists(FiguresDir() / fs::path(name), ec); ++n) {
            name = base + " (" + std::to_string(n) + ").bin";
        }
        const fs::path path = FiguresDir() / fs::path(name);
        std::string err;
        // id >= 1000 = veiculo/gadget; abaixo disso = personagem
        const bool created = id >= 1000 ? host->CreateVehicle(NarrowPath(path), id, err)
                                        : host->CreateFigure(NarrowPath(path), id, err);
        if (!created) {
            return Fail(res, 500, err.empty() ? "could not create the figure file" : err);
        }
        if (const auto slot = SlotParam(req, "slot")) {
            uint32_t placed_id = 0;
            const std::string perr = PlaceFigure(*slot, name, &placed_id);
            NotifyPortalChanged();
            if (!perr.empty()) {
                return Fail(res, 500, perr);
            }
        }
        res.set_content("{\"ok\":true,\"file\":\"" + JsonEscape(name) + "\"}", "application/json");
    });

    srv.Get("/api/loadouts", [this](const httplib::Request&, httplib::Response& res) {
        std::string j = "[";
        for (const Loadout& l : ReadLoadouts()) {
            if (j.size() > 1) {
                j += ',';
            }
            j += "{\"name\":\"" + JsonEscape(l.name) + "\",\"slots\":[";
            for (size_t i = 0; i < l.files.size(); ++i) {
                j += (i ? ",\"" : "\"") + JsonEscape(l.files[i]) + "\"";
            }
            j += "]}";
        }
        j += ']';
        res.set_header("Cache-Control", "no-store");
        res.set_content(j, "application/json");
    });

    srv.Post("/api/loadouts/save", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.get_param_value("name");
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '\t' || c == '\r' || c == '\n'; }),
                   name.end());
        if (name.empty() || name.size() > 60) {
            return Fail(res, 400, "name required");
        }
        Loadout l;
        l.name = name;
        PortalSnapshot snap = SnapshotNow();
        for (int i = 0; i < kSlotCount; ++i) {
            l.files[static_cast<size_t>(i)] = snap.slots[static_cast<size_t>(i)].present ? snap.slots[static_cast<size_t>(i)].file : "";
        }
        auto list = ReadLoadouts();
        auto it = std::find_if(list.begin(), list.end(), [&](const Loadout& x) { return x.name == name; });
        if (it != list.end()) {
            *it = l;
        } else {
            list.push_back(l);
        }
        WriteLoadouts(list);
        Ok(res);
    });

    srv.Post("/api/loadouts/apply", [this](const httplib::Request& req, httplib::Response& res) {
        const size_t i = static_cast<size_t>(std::atoi(req.get_param_value("i").c_str()));
        const auto list = ReadLoadouts();
        if (i >= list.size()) {
            return Fail(res, 400, "unknown loadout");
        }
        const std::string err = ApplyLoadout(list[i]);
        if (!err.empty()) {
            return Fail(res, 400, err);
        }
        Ok(res);
    });

    srv.Post("/api/loadouts/delete", [this](const httplib::Request& req, httplib::Response& res) {
        const size_t i = static_cast<size_t>(std::atoi(req.get_param_value("i").c_str()));
        auto list = ReadLoadouts();
        if (i >= list.size()) {
            return Fail(res, 400, "unknown loadout");
        }
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        WriteLoadouts(list);
        Ok(res);
    });

    srv.Post("/api/loadouts/up", [this](const httplib::Request& req, httplib::Response& res) {
        const size_t i = static_cast<size_t>(std::atoi(req.get_param_value("i").c_str()));
        auto list = ReadLoadouts();
        if (i == 0 || i >= list.size()) {
            return Fail(res, 400, "cannot move");
        }
        std::swap(list[i], list[i - 1]);
        WriteLoadouts(list);
        Ok(res);
    });

    // estaticos: por ultimo, para nao tapar as rotas /api
    srv.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
        if (ServeAsset(req, res)) {
            return;
        }
        res.status = 404;
        res.set_content("not found", "text/plain");
    });
}

bool Core::Impl::Start(ToypadHost* h, const Config& cfg, std::string& err) {
    if (running.load()) {
        return true;
    }
    if (!h) {
        err = "no host";
        return false;
    }
    host = h;
    config = cfg;

    char boot[32];
    std::snprintf(boot, sizeof(boot), "%llx", static_cast<unsigned long long>(
                                                   std::chrono::system_clock::now().time_since_epoch().count()));
    boot_id = boot;

    server = std::make_unique<httplib::Server>();
    RegisterRoutes(*server);

    int port = config.port;
    if (port == 0) {
        port = server->bind_to_any_port(config.bind_address.c_str());
        if (port <= 0) {
            err = "could not bind to any port";
            server.reset();
            return false;
        }
    } else if (!server->bind_to_port(config.bind_address.c_str(), port)) {
        err = "port " + std::to_string(port) + " already in use";
        server.reset();
        return false;
    }
    bound_port = port;
    running.store(true);
    thread = std::thread([this] {
        server->listen_after_bind();
        running.store(false);
    });
    return true;
}

void Core::Impl::Stop() {
    if (server) {
        server->stop();
    }
    if (thread.joinable()) {
        thread.join();
    }
    server.reset();
    running.store(false);
}

// -----------------------------------------------------------------------------------------------

Core& Core::Instance() {
    static Core instance;
    return instance;
}

Core::Core() : m_impl(std::make_unique<Impl>()) {}

Core::~Core() { m_impl->Stop(); }

bool Core::Start(ToypadHost* host, const Config& config, std::string& err) {
    if (!m_impl->Start(host, config, err)) {
        return false;
    }
    const std::string port = std::to_string(m_impl->bound_port);
    std::string lan = LanAddress(false);
    if (lan.empty()) {
        lan = "127.0.0.1";
    }
    m_impl->url = "http://" + lan + ":" + port;
    if (const std::string ts = LanAddress(true); !ts.empty()) {
        m_impl->url += "  |  Tailscale: http://" + ts + ":" + port;
    }
    if (config.announce_in_log && host) {
        host->Log(0, "Toy Pad Remote: abre " + m_impl->url + " no telemovel (figuras: " + host->FiguresDir() + ")");
    }
    return true;
}

void Core::Stop() { m_impl->Stop(); }

bool Core::Running() const { return m_impl->running.load(); }

std::string Core::Url() const { return m_impl->url; }

int Core::Port() const { return m_impl->bound_port; }

void Core::OnGameCommand(const Packet& command) { m_impl->OnGameCommand(command); }

void Core::NotifyPortalChanged() { m_impl->NotifyPortalChanged(); }

void Core::SetSelected(int slot) {
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->selected != slot) {
        m_impl->selected = slot;
        m_impl->Bump();
    }
}

} // namespace toypad_remote
