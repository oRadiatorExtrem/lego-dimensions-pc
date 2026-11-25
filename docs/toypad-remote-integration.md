# Toy Pad Remote — mapa de integração (Cemu ↔ port)

Documento de trabalho. Objetivo: correr o **Toy Pad Remote** (servidor web + web app para telemóvel)
**dentro do port**, arrancado automaticamente com o jogo quando o toggle do launcher está ligado —
tal como já acontece no fork de Cemu — e manter **um único núcleo** compilado pelas duas frentes.

Estado: mapeamento feito a partir do código (sem alterações de comportamento).

---

## 1. O que existe hoje

### 1.1 Fork de Cemu (`F:\LEGO Dimensions\ToyPad-Remote`)

| Item | Detalhe |
|---|---|
| Base | `v2.6`, branch `toypad-remote`, 7 commits à frente (`git describe` = `v2.6-7-g4195f4d`) |
| Remote | `origin` = `cemu-project/Cemu` (upstream) |
| Servidor | `src/Cafe/OS/libs/nsyshid/ToypadRemote.cpp` (959 linhas) + `ToypadRemote.h` (18) |
| HTTP | `dependencies/cpp-httplib/httplib.h` (10 255 linhas, header-only, já vendorizado) |
| Web app | `bin/resources/toypad-remote/` — 17 MB (313 `templates/*.bin` + 377 KB, ~324 PNG em `img/`, `index.html` 450 linhas / 24 KB, `catalog.js` 33 KB, `images.js` 6,6 KB) |
| Config | `%APPDATA%\Cemu\toypad_remote.txt` → `port=8090`, `figures=<userdata>/dimensions` |
| Estado partilhado | `g_dimensionstoypad` (`nsyshid::DimensionsUSB`, `Dimensions.cpp` 1233 linhas) |

Pontos de integração no Cemu (6, todos já no fork):

| # | Ficheiro:linha | Chamada | Papel |
|---|---|---|---|
| 1 | `src/gui/CemuApp.cpp:309` | `nsyshid::ToypadRemote::Start()` | arranca o servidor no arranque da app |
| 2 | `src/Cafe/OS/libs/nsyshid/Dimensions.cpp:548` | `ToypadRemote::OnToypadCommand(buf)` | captura os comandos de LED que o jogo envia ao pad |
| 3 | `src/input/emulated/VPADController.cpp:81` | `ToypadRemote::OnGamepad(...)` | atalhos no comando (Home + botões) e *esconde* esses botões do jogo |
| 4 | `Dimensions.h` | `CreateVehicle`, `IdentifyFigure`, `GetSlots`, `SwapFigures` | API acrescentada ao fork para o servidor |
| 5 | `ToypadRemote.cpp` (resto) | `TempRemove`, `MoveFigure`, `RemoveFigure`, `LoadFigure`, `FindFigure`, `SwapFigures`, `GetSlots` | manipulação do pad |
| 6 | `ActiveSettings::GetDataPath` / `GetUserDataPath` | — | só serve para resolver caminhos e log (`cemuLog_log`) |

### 1.2 Port (`oRadiatorExtrem/lego-dimensions-pc`)

O port **já tem** praticamente toda a maquinaria de pad; o que falta é a camada HTTP.

| Ficheiro | Linhas | O que é |
|---|---|---|
| `src/hle/overrides/hle_toypad.cpp` | 870 | estado do portal (7 posições, 3 zonas), protocolo USB de 32 bytes, TEA, tags NTAG213, LEDs, `portal.json` |
| `src/hle/overrides/hle_toypad.h` | 86 | API pública |
| `src/hle/overrides/hle_toypad_listener.cpp` | 205 | listener TCP `127.0.0.1:9191` para o app LegoToypad (LOAD / REMOVE / MOVE / GET_LED) |
| `src/hle/overrides/hle_toypad_hid.cpp` | — | Toy Pad **físico** por USB/HID |
| `src/hle/overrides/hle_usbd.cpp` | — | `InjectToypadPacket`, `TakeToypadExternalControl`, `RealToypadPacket` |
| `src/hle/overrides/hle_pad.cpp` | 1065 | XInput/SDL/teclado → `struct Input { u32 buttons; lx,ly,rx,ry; l2,r2 }` |
| `src/common/toypad_process.h` | 134 | deteta processo do app LegoToypad |

Escolha de backend hoje (`src/main.cpp:1728-1735`):

```cpp
hle::SetVirtualToypadConnected(toypad);
if (toypad && !selftest && real_toypad && hle::UseRealToypad()) {
    // Toy Pad fisico USB atende o jogo; portal.json e app ficam de fora
} else if (toypad && !selftest) {
    hle::StartPortalWatcher(portal_file);   // relê portal.json a cada 500 ms
    hle::StartToypadListener(toypad_port);  // app LegoToypad, 9191
}
```

Flags: `--toypad` / `--no-toypad`, `--no-real-toypad`, `--toypad-port N`. Launcher: `launcher.ini`
`toypad_auto=1` + `tools\LegoToypad.exe` (`tools/dist/launcher.cpp:456`, `:1742-1745`, `:2736`).

---

## 2. Contrato HTTP do servidor (o que tem de ser replicado)

Servidor `httplib::Server`, `bind_to_port("0.0.0.0", port)` + `listen_after_bind()` em thread *detached*.
Estáticos em `/` a partir de `resources/toypad-remote` (`Cache-Control: no-cache` para `.html`,
`max-age=86400` para o resto).

| Método | Rota | Parâmetros | Efeito |
|---|---|---|---|
| GET | `/api/state` | — | estado completo (JSON abaixo), `no-store` |
| GET | `/api/events` | — | SSE `text/event-stream`; `data: <state>` por mudança, `: ping` a cada 10 s |
| GET | `/api/figures` | — | lista dos `.bin` da pasta de figuras + favoritos + se estão no pad |
| POST | `/api/figures/fav` | `file` | alterna favorito |
| POST | `/api/figures/rename` | `file`, `to` | renomeia ficheiro |
| POST | `/api/figures/delete` | `file` | apaga ficheiro |
| POST | `/api/place` | `slot` 0-6, `file` | põe figura (move se a tag já estiver no pad) |
| POST | `/api/remove` | `slot` | tira figura |
| POST | `/api/zone` | `slot`, `pad` 1-3 | move para zona (primeiro livre, senão troca) |
| POST | `/api/move` | `from`, `to` | move entre posições |
| POST | `/api/create` | `id` (u32), `name`, `slot?` | cria `.bin` (template `<id>.bin` se existir; senão `CreateVehicle`/`CreateFigure`) |
| GET | `/api/loadouts` | — | loadouts de `<figures>/loadouts.txt` |
| POST | `/api/loadouts/{save,apply,delete,up}` | `name`, `id` | gerir loadouts |

Payload de estado:

```json
{"boot":"<epoch>","v":<uint>,"awake":<bool>,"sel":<int -1..6>,
 "slots":[null | {"id":<u32>,"file":"<nome>.bin"} ×7],
 "leds":[{"m":"c|f|F|r","c":[r,g,b],"p":[p1,p2,p3],"s":<seq>} ×3]}
```

Geometria do pad (igual nos dois lados, obrigatório manter):

```cpp
SLOT_PAD  = {2, 1, 3, 2, 2, 3, 3};        // posição 0..6 -> zona
PAD_SLOTS = {{1}, {0, 3, 4}, {2, 5, 6}};  // zona -> posições
PAD_NAME  = {"Centro", "Esquerda", "Direita"};
```

LED: `mode` `'c'` cor, `'f'` fade, `'F'` flash, `'r'` fade aleatório; `seq` incrementa a cada animação
para o cliente reiniciar a animação. Comandos capturados em `OnToypadCommand`:
`0xB0` wake · `0xC0` cor · `0xC2` fade · `0xC3` flash · `0xC4` fade aleatório ·
`0xC6` fade all · `0xC7` flash all · `0xC8` cor all.

---

## 3. Gap analysis (Cemu → port)

| Capacidade do servidor | Cemu | Port | Trabalho |
|---|---|---|---|
| Servir estáticos + REST + SSE | ✅ | ❌ não há servidor HTTP | vendorizar `cpp-httplib`, +2 ficheiros |
| `GetSlots()` (7 posições, id por posição) | ✅ | ❌ interno (`g_figures`, sem accessor) | expor `Slots()` em `hle_toypad.h` |
| `SwapFigures()` | ✅ | ❌ | implementar (≈15 linhas sobre `MoveIndex`) |
| `MoveFigure` / `TempRemove` / `RemoveFigure` | ✅ | ✅ `MoveIndex`, `RemoveIndex` | adaptador |
| `LoadFigure(data, file, pad, index)` | ✅ | ✅ `LoadTag(pad, index, tag, path, &id)` | adaptador (o port **guarda o caminho** na tag → melhor) |
| `TempRemove` + 500 ms + `LoadFigure` | ✅ | ✅ igual no listener TCP (`kPickupDelay`) | reutilizar |
| Injeção de eventos no jogo | `SendCommand` interno | ✅ `InjectToypadPacket` + `TakeToypadExternalControl` | reutilizar |
| Captura de LEDs → estado | `SendCommand` → `OnToypadCommand` | ✅ mas para o *app* em formato v2 (`LedSnapshot`, 40 bytes) | novo accessor estruturado (r,g,b,modo,params,seq) |
| `CreateFigure` / `CreateVehicle` | ✅ | ❌ | implementar (o port tem `BuildTag` + TEA; falta a escrita do `.bin` com upgrades) |
| `FindFigure(id)` (nome) | ✅ (`GetListMinifigs`/`GetListTokens`) | ❌ sem tabela de nomes | o port pode usar o **nome do ficheiro** (já guarda `path`) + tabela opcional |
| Atalhos Home + botões | ✅ `VPADController` | ❌ | hook em `hle_pad.cpp` sobre `struct Input` |
| Config (`port`, `figures`) | `toypad_remote.txt` | — | `launcher.ini` + flags CLI |
| Templates de figuras (313 `.bin`) | ✅ | ❌ | copiar para `assets/toypad_remote/templates` |

---

## 4. Arquitetura proposta — um núcleo, dois anfitriões

Não é um executável à parte: é **um par de ficheiros de código** compilado para dentro de cada host.

```
toypad-remote-core/                 (novo, partilhado pelas duas frentes)
├── ToypadRemoteCore.h              # estado, servidor, JSON, SSE, loadouts, ficheiros
├── ToypadRemoteCore.cpp
├── ToypadHost.h                    # interface abstrata que cada host implementa
├── httplib.h                       # vendorizado (o Cemu já o tem)
└── web/                            # index.html, catalog.js, images.js, manifest.json, img/, templates/
```

Interface de host — **única** superfície que cada lado tem de implementar:

```cpp
struct RemoteSlot { bool present; uint32_t id; std::string file; };

class ToypadHost {
public:                                   // leitura
    virtual std::array<RemoteSlot, 7> Slots() = 0;
    virtual std::array<LedState, 3>   Leds() = 0;
    virtual bool SessionReady() = 0;
public:                                   // escrita (o núcleo já aplicou a ordem e o intervalo de 500 ms)
    virtual bool LoadTag(int pad, int index, const TagData&, const std::string& path, uint32_t* id) = 0;
    virtual bool RemoveIndex(int index) = 0;
    virtual std::vector<Packet> MoveIndex(int pad, int index, int oldIndex) = 0;
    virtual bool Swap(int padA, int a, int padB, int b) = 0;
    virtual bool CreateFigure(const std::string& path, uint32_t id) = 0;
    virtual bool CreateVehicle(const std::string& path, uint32_t id) = 0;
    virtual void Announce(const std::vector<Packet>&) = 0;   // injeta no jogo (respeita SessionReady)
    virtual std::string FindName(uint32_t id) = 0;
public:                                   // caminhos/telemetria
    virtual std::string DataDir() = 0;
    virtual void Log(const char* fmt, ...) = 0;
};
```

Adaptadores:

* **Cemu** (`nsyshid::CemuToypadHost`): delega em `g_dimensionstoypad` — o código do fork passa a ser
  uma casca fina. `OnToypadCommand` continua no adaptador e empurra para `core.OnLedCommand()`.
* **Port** (`hle::RecompToypadHost`): delega em `hle::toypad::*` + `InjectToypadPacket` /
  `TakeToypadExternalControl`.

Consequência: **um único bug fix serve as duas frentes**, e o web app (o mesmo `index.html`) funciona
sem alterações porque o contrato HTTP é idêntico.

---

## 5. Auto-start com o jogo (toggle)

Launcher (`tools/dist/launcher.cpp`), secção **Settings → Portal**:

| Opção | `launcher.ini` | Efeito |
|---|---|---|
| Portal na tela (LegoToypad app) | `toypad_auto=1` | abre `tools\LegoToypad.exe` (já existe) |
| **Toy Pad Remote** | `toypad_remote=1` | arranca o servidor **dentro do jogo** |
| Porta | `remote_port=8090` | 0 = porta livre à escolha do sistema |
| Pasta de figuras | `remote_figures=<path>` | vazio = `assets\toypad_remote\figures` |
| PIN (opcional) | `remote_pin=` | exigido nas escritas |

No arranque (`src/main.cpp`, junto ao bloco do portal de `:1728`):

```cpp
if (toypad && !selftest && toypad_remote) {
    hle::toypadremote::Start({.port = remote_port, .figures = remote_figures, .pin = remote_pin});
    // imprime no log e na consola: "Toy Pad Remote: abre http://192.168.x.x:8090 no telemóvel"
}
```

Regras de coexistência com os backends existentes (a decidir, proposta):

1. Toy Pad **físico** ligado → remote não arranca (o físico manda), aviso no log.
2. App LegoToypad a correr → remote arranca **na mesma** (portas diferentes: 8090 HTTP vs 9191 TCP);
   o estado do pad é o mesmo, os dois controlam o mesmo portal. Sem conflito porque ambos passam por
   `InjectToypadPacket`.
3. `portal.json` → passa a ser **espelho de leitura** quando o remote está ligado (o remote escreve o
   mesmo ficheiro), para não haver duas fontes de verdade a competir no *watcher* de 500 ms.

---

## 6. Otimizações a fazer na versão nova (defeitos lidos no código atual)

| # | Problema | Onde | Correção |
|---|---|---|---|
| 1 | `s_cv.notify_all()` + `StateJson()` completo por cada comando de LED; com vários telemóveis ligados e o jogo a animar zonas, são dezenas de JSON/s | `ToypadRemote.cpp:673-691` (SSE) + `:58` (`notify_all`) | coalescer: enviar no máximo a cada 50 ms e só quando `v` mudou desde o último envio **daquele** cliente |
| 2 | `ReadFavorites()` lê o disco em cada `GET /api/figures` | `:698` | cache em memória invalidada só nas escritas |
| 3 | `StateJson()` corre com `s_mutex` e chama `GetSlots()`, que tranca `m_dimensionsMutex` → dois mutexes encadeados (risco de inversão com `Dimensions.cpp`) | `:161-200` | copiar o estado com um único lock no adaptador e serializar fora do lock |
| 4 | `s_slotFile` só em memória → depois de reiniciar, o pad mostra figuras **sem nome** | `:48` | o port já persiste o caminho na tag; usar isso como fonte |
| 5 | Sem `gzip`: `index.html` 24 KB + `catalog.js` 33 KB por arranque a frio no telemóvel | estáticos | `set_pre_compressed_file_handler` / compressão na build do web app |
| 6 | 17 MB de PNG carregados de uma vez na biblioteca | `img/` | lazy-load por `IntersectionObserver` + WebP (≈70 % menos bytes) |
| 7 | Sem autenticação nem verificação de `Origin` → qualquer página aberta no PC pode `POST /api/place` | todas as rotas POST | token de sessão no URL (`?k=`) + recusar `Origin` externo nas escritas |
| 8 | `bind 0.0.0.0` em rede pública | `:944` | manter, mas mostrar o URL só para adaptadores privados + Tailscale (já há deteção de `100.64/10`) |
| 9 | Log por comando a `LogType::Force` | `:956` | nível normal; debug atrás de flag |
| 10 | `s_selected` (atalhos) não sobrevive a reinícios nem é por jogador | `:50` | por jogador (1-4) quando se ligar o co-op |

---

## 7. Plano por fases

| Fase | Entrega | Ficheiros | Verificação |
|---|---|---|---|
| **F0** | Núcleo + interface de host, compila nos dois lados, sem comportamento | `toypad-remote-core/*`, adaptador Cemu a compilar | Cemu continua igual; `GET /api/state` responde |
| **F1** | Port: espelho só de leitura (posições + LEDs no telemóvel) | `hle_toypad.h/.cpp` (novos accessors), `hle_toypadremote.cpp` | telemóvel vê as 3 figuras e as cores mudarem ao vivo |
| **F2** | Port: escrita (place/remove/move/zone) com o intervalo de 500 ms | adaptador usa `LoadTag`/`RemoveIndex`/`MoveIndex` + `InjectToypadPacket` | colocar/tirar do telemóvel funciona no jogo, como no Cemu |
| **F3** | Biblioteca de figuras (listar, favoritos, renomear, apagar, criar) | `CreateFigure`/`CreateVehicle` novos, templates copiados | criar veículo e pôr no pad |
| **F4** | Loadouts (guardar, aplicar por botão, A/B/X/Y) | núcleo | loadout repõe o pad |
| **F5** | Launcher: toggle + auto-start + `launcher.ini` + coexistência | `tools/dist/launcher.cpp`, `main.cpp` | liga o toggle, arranca o jogo, servidor no ar |
| **F6** | Atalhos no comando (Home/PS + setas) | `hle_pad.cpp` (hook antes de entregar o `Input`) | botões escondidos do jogo, figura move |
| **F7** | Endurecimento + otimizações da §6 | núcleo | 4 telemóveis ligados, CPU < 1 % |
| **F8** | Sincronizar o núcleo de volta ao fork de Cemu | `ToypadRemote.cpp` → casca do adaptador | paridade funcional |

Ordem de risco crescente; F1+F2 já dão o valor principal (controlar o portal do telemóvel).

---

## 8. Riscos e armadilhas

1. **Duas fontes de verdade no pad.** `portal.json` + `StartPortalWatcher` (500 ms) + remote a escrever
   pode criar figuras fantasma. Resolver antes de F2 (§5.3).
2. **O intervalo de 500 ms não é cosmético.** Sem ele, "colocada" e "removida" no mesmo instante fazem o
   jogo perder a figura (comentário em `hle_toypad_listener.cpp:52-57`). O núcleo tem de o impor, não o host.
3. **`SessionReady()`**: eventos antes de WAKE + desafio são perdidos. O núcleo tem de os reter e
   reenviar em `PresentEvents()` (o port já tem a vigia).
4. **O comando PS/Home não existe no port.** O port mapeia `XINPUT_GAMEPAD_*` + teclado; é preciso
   escolher o botão modificador (proposta: `kTouchPad` ou `Options` mantido + `L3`).
5. **Atualização do Cemu.** O fork está 7 commits à frente de `v2.6`; qualquer rebase em cima de Cemu
   novo tem de reaplicar 6 pontos de integração mínimos — razão adicional para os reduzir a *um*
   (o adaptador) já em F0.
6. **Caminhos ANSI.** O port recebe caminhos em ANSI (README): a pasta de figuras não pode ter acentos.

---

## 9. Decisões em aberto

1. Onde vive o núcleo partilhado: repo novo (`toypad-remote-core`) + cópia por script, ou submodule?
   (cópia+script é mais simples e não obriga a rede na build)
2. Manter a dependência `cpp-httplib` ou escrever o servidor sobre o `winsock` que o port já usa no
   listener 9191 (menos 10 k linhas, mais trabalho)?
3. PIN obrigatório nas escritas por omissão, ou opcional?
4. O remote substitui o app LegoToypad no launcher (opções exclusivas) ou convivem os dois?
5. Nomes das figuras: tabela de ids (como o Cemu) ou nome do ficheiro (como o port já guarda)?
