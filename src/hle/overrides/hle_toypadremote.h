#pragma once

#include <filesystem>
#include <string>

#include "common/types.h"

// Toy Pad Remote dentro do port: servidor HTTP + web app para controlar o portal pelo telemovel.
// O servidor em si vive no nucleo partilhado (third_party/toypad-remote-core, o mesmo que o Cemu
// usa); aqui esta so a ponte para hle::toypad.
namespace hle::toypad_remote {

// Arranca o servidor. `figures_dir` e a pasta dos .bin (criada se faltar). `pin` vazio = sem PIN.
// Devolve false e o motivo em `err` se a porta estiver ocupada.
bool Start(int port, const std::string& pin, const std::filesystem::path& figures_dir, std::string* err);

void Stop();
bool Running();
// "http://192.168.1.20:8090" (vazio se nao estiver a correr)
std::string Url();

// ---------------------------------------------------------------- atalhos no comando
// Aplica os atalhos do Toy Pad aos botoes de um jogador (0 = jogador 1). Devolve os botoes que o
// jogo deve ver: com o modificador em baixo os botoes sao consumidos (o jogo nao os recebe).
//
//   Modificador: TouchPad / Share (o botao menos usado no jogo; o Options junta o jogador 2 no co-op)
//   + Esquerda / Cima / Direita   move a figura selecionada para a zona esquerda / centro / direita
//   + Baixo / R1                  escolhe a figura seguinte / anterior
//   + Cruz                        move a selecionada para a zona que o jogo esta a piscar
//   + Quadrado / Circulo / Triangulo / L1   aplica o loadout 1 / 2 / 3 / 4
u32 HandleGamepad(int player, u32 buttons);

// Esconde do jogo os botoes de atalho enquanto o modificador esta em baixo (por jogador).
bool GamepadConsumesButtons(int player, u32 buttons);

} // namespace hle::toypad_remote
