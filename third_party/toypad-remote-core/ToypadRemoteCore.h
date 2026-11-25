#pragma once

// ToypadRemoteCore — servidor HTTP + web app do Toy Pad Remote, partilhado pelo Cemu e pelo port.
//
// Nao e um executavel: e compilado para dentro de cada anfitriao. O anfitriao implementa ToypadHost
// e chama Start(); o nucleo trata de tudo o resto.
//
// Contrato HTTP (identico ao do fork de Cemu, para o mesmo web app servir os dois):
//   GET  /api/state           estado completo (JSON)
//   GET  /api/events          Server-Sent Events; data: <estado> a cada mudanca (coalescido a 50 ms)
//   GET  /api/figures         .bin da pasta + favoritos + se estao no pad
//   POST /api/figures/fav     file, on=0|1
//   POST /api/figures/rename  file, to
//   POST /api/figures/delete  file
//   POST /api/place           slot=0..6, file
//   POST /api/remove          slot
//   POST /api/zone            slot, pad=1..3
//   POST /api/move            from, to
//   POST /api/create          id, name, slot?
//   GET  /api/loadouts        lista
//   POST /api/loadouts/save|apply|delete|up   name / i
//
// Seguranca (sempre ativa):
//   * todos os POST exigem o cabecalho "X-TPR: 1";
//   * o "Origin", quando presente, tem de ser a propria origem do servidor;
//   * "OPTIONS" e sempre recusado -> nenhum site externo consegue fazer o preflight de um POST.
//   * PIN opcional (Config::pin), desligado por omissao: cabecalho "X-TPR-Pin" ou ?pin=.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "ToypadHost.h"

namespace toypad_remote {

struct Config {
    int port = 8090;             // 0 = porta escolhida pelo sistema
    std::string pin;             // vazio = sem PIN (so para quem expoe fora da rede de casa)
    std::string web_dir;         // opcional: pasta em disco que ganha ao site embutido (desenvolvimento)
    // Pasta de dados opcional do utilizador: `img/<id>.png` (icones) e `templates/<id>.bin`.
    // Nada disto e' distribuido com o projeto; se faltar, o web app mostra um marcador com o id.
    std::string data_dir;
    std::string bind_address = "0.0.0.0";
    bool announce_in_log = true; // escreve o URL no log do anfitriao ao arrancar
};

class Core {
  public:
    static Core& Instance();

    // Arranca o servidor numa thread propria. Devolve false (e o motivo em `err`) se a porta
    // estiver ocupada. Chamadas repetidas com o servidor ja a correr devolvem true.
    bool Start(ToypadHost* host, const Config& config, std::string& err);
    void Stop();
    bool Running() const;

    // URL para o telemovel, ex.: "http://192.168.1.20:8090"; vazio se nao estiver a correr.
    std::string Url() const;
    int Port() const;

    // ---------------------------------------------------------------- entrada do anfitriao
    // Cada comando de 32 bytes que o jogo envia ao pad (o mesmo ponto onde o Cemu chama
    // OnToypadCommand e o port tem os LEDs em HandleLedCommand). Trata 0xB0 e 0xC0..0xC8.
    void OnGameCommand(const Packet& command);

    // Qualquer mudanca no portal, venha de onde vier (remote, app LegoToypad, portal.json,
    // Toy Pad fisico, atalho no comando). Unico ponto de notificacao: gera evento SSE.
    void NotifyPortalChanged();

    // Posicao selecionada pelos atalhos no comando (-1 = nenhuma).
    void SetSelected(int slot);

    // ---------------------------------------------------------------- atalhos no comando
    // Quantos loadouts existem (para os atalhos A/B/X/Y poderem mostrar o nome).
    int LoadoutCount();
    // Aplica o loadout `index` (ordem do loadouts.txt). Devolve false e o motivo em `err`.
    bool ApplyLoadout(int index, std::string* err);
    // Nome do loadout `index` ("" se nao existir).
    std::string LoadoutName(int index);
    // Zona (1 centro, 2 esquerda, 3 direita) que o jogo esta a piscar agora (0 = nenhuma). O jogo
    // pisca uma zona quando quer uma figura la: e o alvo natural do atalho "mover para onde pisca".
    int BlinkingZone();

  private:
    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace toypad_remote
