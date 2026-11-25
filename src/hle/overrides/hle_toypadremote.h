#pragma once

#include <filesystem>
#include <string>

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

} // namespace hle::toypad_remote
