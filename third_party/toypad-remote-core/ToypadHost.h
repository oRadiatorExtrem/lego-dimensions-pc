#pragma once

// ToypadHost — a unica superficie que cada anfitriao (Cemu, port) tem de implementar.
//
// REGRAS DE LOCKS (obrigatorias, ver README):
//   Ordem unica permitida:  1.o lock do servidor (do nucleo)  2.o lock do pad (do anfitriao)
//   Snapshot() deve copiar os valores e LARGAR todos os locks antes de voltar:
//   o nucleo formata JSON e serve HTTP depois, sem locks do pad na mao.
//   LoadTag/RemoveIndex/MoveIndex/Swap/Create* nao podem pedir locks do nucleo.
//   Announce() e chamado sem locks do anfitriao.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace toypad_remote {

constexpr int kSlotCount = 7; // posicoes do portal (indice 0..6 no protocolo)
constexpr int kPadCount = 3;  // zonas: 1 = centro, 2 = esquerda, 3 = direita
constexpr int kTagBytes = 0x2D * 4; // 180: NTAG213, 45 paginas de 4 bytes

using Tag = std::array<uint8_t, kTagBytes>;
using Packet = std::array<uint8_t, 32>;

// Geometria do pad. Igual nos dois anfitrioes; o web app tem a mesma tabela.
constexpr std::array<uint8_t, kSlotCount> kSlotPad = {2, 1, 3, 2, 2, 3, 3};
constexpr std::array<std::array<uint8_t, 3>, kPadCount> kPadSlots = {{{1, 255, 255}, {0, 3, 4}, {2, 5, 6}}};

// Estado de um LED de zona, ja convertido para o formato do web app.
struct LedState {
    char mode = 'o'; // 'o' apagado, 'c' cor fixa, 'f' fade, 'F' flash, 'r' fade aleatorio
    uint8_t r = 0, g = 0, b = 0;
    // 'f': p1 = velocidade, p2 = ciclos        'F': p1 = aceso, p2 = apagado, p3 = ciclos
    // 'r': p1 = velocidade, p2 = ciclos
    uint8_t p1 = 0, p2 = 0, p3 = 0;
    uint32_t seq = 0; // incrementa a cada animacao nova (o cliente reinicia a animacao)
};

struct SlotState {
    bool present = false;
    uint32_t id = 0;      // id da figura (fiavel: vem da tag)
    std::string file;     // nome do ficheiro (o que o utilizador ve e pode renomear)
};

// Copia do estado do portal num instante. Sem locks: e isto que o nucleo serializa.
// O anfitriao preenche `slots`, `session_ready` e `awake`; o nucleo sobrepoe `leds` e `selected`
// (que sao estado do servidor, alimentado pelos comandos do jogo).
struct PortalSnapshot {
    bool awake = false;
    bool session_ready = false; // false = o jogo ainda nao fez WAKE + desafio
    int selected = -1;          // posicao dos atalhos no comando (-1 = nenhuma)
    std::array<SlotState, kSlotCount> slots{};
    std::array<LedState, kPadCount> leds{};
};

class ToypadHost {
  public:
    virtual ~ToypadHost() = default;

    // ---------------------------------------------------------------- leitura
    // Copia do estado. Chamado com frequencia (SSE): tem de ser barato e nao bloquear o jogo.
    virtual PortalSnapshot Snapshot() = 0;

    // ---------------------------------------------------------------- escrita
    // Todas devolvem false e preenchem `err` (texto para o utilizador) em caso de falha.
    // `file` e o nome do .bin dentro de FiguresDir() (sem caminho).
    virtual bool LoadTag(int pad, int index, const Tag& tag, const std::string& file, uint32_t* out_id, std::string& err) = 0;
    virtual bool RemoveIndex(int index, std::string& err) = 0;
    virtual bool MoveIndex(int pad, int index, int old_index, std::string& err) = 0;
    virtual bool Swap(int pad_a, int index_a, int pad_b, int index_b, std::string& err) = 0;

    // Cria o ficheiro da figura/veiculo em `path` (caminho completo) com o id dado.
    virtual bool CreateFigure(const std::string& path, uint32_t id, std::string& err) = 0;
    virtual bool CreateVehicle(const std::string& path, uint32_t id, std::string& err) = 0;

    // ---------------------------------------------------------------- jogo
    // Entrega os pacotes de evento (figura colocada/removida) ao jogo, na ordem e com o
    // intervalo de 500 ms que o nucleo ja aplicou. Chamado sem locks.
    virtual void Announce(const std::vector<Packet>& packets) = 0;

    // ---------------------------------------------------------------- nomes/catalogo
    // Id de uma tag (0 se nao der para ler). O nucleo le o .bin e pede isto ao anfitriao.
    virtual uint32_t IdentifyFigure(const Tag& tag) = 0;
    // Nome de catalogo para um id ("" se desconhecido). O nucleo usa como fallback do nome.
    virtual std::string CatalogName(uint32_t id) = 0;

    // ---------------------------------------------------------------- caminhos/telemetria
    // Pasta onde estao e sao criados os .bin. Garantida existente pelo anfitriao.
    virtual std::string FiguresDir() = 0;
    // level: 0 = info, 1 = aviso, 2 = erro
    virtual void Log(int level, const std::string& message) = 0;
};

} // namespace toypad_remote
