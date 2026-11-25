# Toy Pad Remote — mapa de integração (Cemu ↔ port)

Documento de trabalho. Objetivo: correr o **Toy Pad Remote** (servidor web + web app para telemóvel)
**dentro do port**, arrancado automaticamente com o jogo quando o toggle do launcher está ligado —
tal como já acontece no fork de Cemu — e manter **um único núcleo** compilado pelas duas frentes.

Estado: **F0, F1 e F2 concluídos.**

* **F0** — núcleo em `oRadiatorExtrem/toypad-remote-core`, sincronizado para o Cemu e verificado a
  correr (`GET /api/state` = 200 dentro do `Cemu_release.exe`); 37 testes verdes no núcleo.
* **F1+F2** — servidor a correr **dentro do port** (`--toypad-remote`), com leitura e escrita
  verificadas no jogo real a partir do telemóvel (branch `feat/toypad-remote-f1-f2`, commit `5729793`).
  Segurança testada no binário: POST sem `X-TPR` → 403, `Origin` externo → 403, `OPTIONS` → 403.

**F3** concluído também: toggle no launcher, janela de porta/PIN, e o núcleo anuncia
`http://<nome-da-maquina>:8090` — com Tailscale é exactamente o mesmo link do Cemu
(`http://legion5-15arh05:8090`), com ou sem maiúsculas no `Origin`.

**F4** concluído: `vehicle_upgrades.tsv` (239 veículos/gadgets, páginas 35/37 extraídas dos
nossos próprios dumps — só números), `catalog_data.h` (nomes e mundos, do LD-ToyPad-Emulator,
MIT) e ícones servidos de `assets/toypad_remote/img` com `/images.js` gerado. O mesmo ícone do
Cemu, sem o projeto distribuir imagens.

**F5/F6** concluídos: atalhos no comando por jogador (modificador TouchPad) com loadouts
□○△/R2, zonas ←↑→, ciclo ↓/R1/L1 e ✕ = mover para a zona que o jogo pisca; selector
**exclusivo** de portal no launcher (Telemóvel / Na tela / Nenhum).

Falta F7 (endurecimento) e F8.

---

## 1. O que existe hoje

### 1.1 Fork de Cemu (`F:\LEGO Dimensions\ToyPad-Remote`)

| Item | Detalhe |
|---|---|
| Base | `v2.6`, branch `toypad-remote`, 7 commits à frente (`git describe` = `v2.6-7-g4195f4d`) |
| Remote | `origin` = `cemu-project/Cemu` (upstream) |
| Servidor | `src/Cafe/OS/libs/nsyshid/ToypadRemote.cpp` (959 linhas) + `ToypadRemote.h` (18) |
| HTTP | `dependencies/cpp-httplib/httplib.h` (10 255 linhas, header-only, já vendorizado) |
| Web app | `bin/resources/toypad-remote/` — 17 MB (313 `templates/*.bin`, ~324 PNG em `img/`, `index.html` 450 linhas / 24 KB, `catalog.js` 33 KB, `images.js` 6,6 KB). **`templates/` e `img/` são só da fork e não podem ser redistribuídos** (§3.1) |
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
| `CreateFigure` / `CreateVehicle` | ✅ | ❌ | implementar (o port tem `BuildTag` + TEA) com a **tabela de melhorias** da §3.1 |
| Catálogo de figuras (mundo, habilidades, imagem) | ✅ `catalog.js` | ❌ | dados próprios, sem imagens de terceiros (§3.1) |
| Identificação: ID + nome | ✅ `IdentifyFigure` + `FindFigure` | parcial (o port tem `TagCharacterId`, guarda `path`) | **os dois, com papéis diferentes** (§3.2) |
| Atalhos Home + botões | ✅ `VPADController` | ❌ | hook em `hle_pad.cpp` sobre `struct Input` |
| Config (`port`, `figures`) | `toypad_remote.txt` | — | `launcher.ini` + flags CLI |
| Templates de figuras (313 `.bin` em `templates/`) | ✅ (só na fork) | ❌ | **não redistribuir** — gerar a tag; ver §3.1 |

---

## 3.1 Redistribuição (bloqueador para contribuir no upstream)

O port é público e GPL-2.0. Na tua fork não há problema; num PR para `vascon-es/lego-dimensions-pc` há.
Duas coisas do fork atual **não podem entrar**:

| Não entra | Porque | Substituto |
|---|---|---|
| `templates/*.bin` (313 dumps do repo `harrysof/LegoToypad`) | dumps de tags de terceiros | **tabela pequena** com os valores de melhorias de fábrica por ID de veículo → a tag é **gerada** |
| `img/*.png` (~324) e `images.js` | imagens da wiki (Fandom) | **download opcional** feito pelo próprio utilizador com o script |

### Melhorias de fábrica por ID de veículo

O "Please update to access the upgrades for this item" vem de criar veículos com o valor `0xEFFFFFFF`
ou `0` nas páginas de melhorias (35/37) em vez dos valores de fábrica. Em vez de distribuir os `.bin`,
basta uma tabela id → valores:

```cpp
// nucleo: tabela de dados, sem ficheiros de terceiros
// { id, page35, page37 }  — extraída da nossa própria observação dos dumps que já temos
constexpr VehicleUpgrades kVehicleUpgrades[] = {
    {0x3D4A36C0 /* DeLorean */, 0xE0FB1C00, 0x00000000},
    /* ... */
};
```

`CreateVehicle(path, id)` continua a **gerar** a tag a partir do UID + id (como o port já faz com
`BuildTag`/TEA) e passa a escrever as melhorias desta tabela. Sem `templates/`, sem `.bin` alheios, e o
bug do "Please update…" fica resolvido. A tabela é factual (valores numéricos por ID), não é conteúdo
artístico de ninguém.

### Imagens

Fora do executável e fora do repositório. O script (`tools/fetch-images.py`, já existe no
ToyPad-Remote como `tools/fetch-images.js`) corre na máquina do utilizador, descarrega para
`assets/toypad_remote/img/` e o servidor serve de lá se a pasta existir. Sem pasta, o web app mostra um
placeholder com o ID — continua utilizável.

### Catálogo

`catalog.js` hoje é gerado a partir dos dados do LD-ToyPad-Emulator (Berny23, MIT) + wiki. Manter só o
que é factual e verificável: ID, mundo, habilidades, tipo (personagem/veículo/gadget). O que vier de
MIT pode ficar, com o aviso de licença em `THIRD_PARTY.md`.

## 3.2 Nomes: os dois, com papéis diferentes (decidido)

Como já é feito no Cemu:

| Campo | Papel | Fiabilidade |
|---|---|---|
| **ID** (u32) | identifica a figura; liga ao catálogo (mundo, habilidades, imagem, modelo) | **fiável** — vem da tag |
| **nome do ficheiro** | o que o utilizador vê e pode renomear | pode estar duplicado ou mudado |

Regra: o ID nunca depende do nome; o nome é só apresentação. Se o nome faltar, o servidor devolve o
nome do catálogo para esse ID; se o ID não estiver no catálogo, mostra o ID em hexadecimal.

---

## 4. Arquitetura — um núcleo, dois anfitriões

Não é um executável à parte: é **um par de ficheiros de código** compilado para dentro de cada host.

### 4.0 Distribuição do núcleo (decidido)

Repositório **próprio** (`github.com/oRadiatorExtrem/toypad-remote-core`, privado por agora), copiado para cada projeto por um script de
sincronização (o mesmo papel de um `git subtree`, mais simples de ler num PR). **Sem submodule:**
complica a vida a quem contribui e os mantenedores do port dificilmente aceitam uma dependência
externa. O commit sincronizado fica registado num ficheiro **`VERSION`** (hash + data) em cada host,
para se saber sempre que versão do núcleo está lá dentro; o script recusa sincronizar com a árvore suja.

```
toypad-remote-core/                 (repo próprio; copiado, não submodulado)
├── VERSION                         # commit do núcleo copiado para o host
├── ToypadRemoteCore.h              # estado, servidor, JSON, SSE, loadouts, ficheiros
├── ToypadRemoteCore.cpp
├── ToypadHost.h                    # interface abstrata que cada host implementa
├── httplib.h                       # vendorizado (MIT, header-only, o Cemu já o usa)
├── web_src/                        # fonte do site (dev): index.html, catalog.js, images.js, manifest.json
├── web_embed.py                    # gera web_assets.h (site dentro do executável)
└── gen_web_assets.h                # (gerado) bytes dos estáticos + tabela de rotas
```

**O site vai embutido no executável** (`web_assets.h`, gerado na build a partir de `web_src/`):
nada de ficheiros em falta numa instalação, nada de `resources/` obrigatório. A pasta em disco fica
como **alternativa de desenvolvimento** — se existir, ganha ao embutido, para se poder iterar no
`index.html` sem recompilar.

Servidor: **`cpp-httplib`**, isolado dentro do núcleo (só o núcleo conhece o `httplib.h`; nenhum
header público o inclui). Trata de SSE, *chunked*, keep-alive e limites de ligação; escrever HTTP à
mão sobre o `winsock` do listener 9191 daria muitos bugs para poupar pouco.

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

### 4.1 Ordem dos locks (documentar e manter)

Hoje **não há inversão** no Cemu: a ordem é sempre a mesma e nada a contradiz.

```
1.º  s_mutex            (ToypadRemote.cpp — estado do servidor: LEDs, seleção, versão/SSE)
2.º  m_dimensionsMutex  (Dimensions.cpp — figuras do pad)
```

* `OnToypadCommand` (código do jogo) só toca no `s_mutex` e **larga-o** antes de o Toy Pad usar o seu.
* `Dimensions.cpp` nunca pede o `s_mutex`.

O que isto é, então, é **fragilidade**: basta alguém pedir os dois na ordem contrária para travar tudo,
e é invisível numa revisão. Regras para o núcleo:

1. O núcleo **nunca** tranca nos dois sentidos: a ordem acima é escrita num comentário no topo de
   `ToypadRemoteCore.h` e é a única permitida.
2. `StateJson()` sai de dentro do `s_mutex`: o adaptador copia os valores para uma `struct` simples
   (com os locks que precisar, pela ordem acima), **larga tudo**, e só depois o núcleo formata o JSON.
   O mesmo para `Slots()`, `Leds()` e a serialização SSE.
3. Os adaptadores não seguram locks do host enquanto chamam `Announce()` (injeção no jogo).

### 4.2 Segurança (decidido)

O ponto mais importante do mapa. Um PIN não resolve o problema real (CSRF: qualquer página aberta no
PC pode fazer `POST /api/place` para `127.0.0.1:8090`). A solução barata, **sempre obrigatória**:

* todos os `POST` exigem o cabeçalho **`X-TPR: 1`**;
* verificação de **`Origin`**: recusar qualquer `Origin` que não seja a própria origem do servidor
  (ou ausente, para clientes não-browser);
* como o cabeçalho próprio força um *preflight* CORS e o servidor **recusa** o `OPTIONS`, nenhum site
  externo consegue enviar o pedido.

Isto **não muda nada na experiência**: o web app embutido já envia o cabeçalho.

**PIN opcional, desligado por omissão** — só para quem expõe o servidor para fora da rede de casa
(redirecionamento de portas). Com Tailscale não é preciso: a rede já está protegida.

O launcher mostra sempre o aviso **"visível na rede local"** junto ao toggle, com o URL real
(adaptador privado + Tailscale).

---

## 5. Auto-start com o jogo (toggle)

Launcher (`tools/dist/launcher.cpp`), secção **Settings → Portal**:

| Opção | `launcher.ini` | Efeito |
|---|---|---|
| Portal na tela (LegoToypad app) | `toypad_auto=1` | abre `tools\LegoToypad.exe` (já existe) |
| **Toy Pad Remote** | `toypad_remote=1` | arranca o servidor **dentro do jogo** |
| Porta | `remote_port=8090` | 0 = porta livre à escolha do sistema |
| Pasta de figuras | `remote_figures=<path>` | vazio = `assets\toypad_remote\figures` |
| PIN (opcional, desligado) | `remote_pin=` | exigido nas escritas; só para quem expõe fora de casa |

Com o toggle ligado o launcher mostra sempre o aviso **"visível na rede local"** e o URL real.

No arranque (`src/main.cpp`, junto ao bloco do portal de `:1728`):

```cpp
if (toypad && !selftest && toypad_remote) {
    hle::toypadremote::Start({.port = remote_port, .figures = remote_figures, .pin = remote_pin});
    // log e consola: "Toy Pad Remote: abre http://192.168.x.x:8090 no telemóvel"
}
```

### 5.1 Coexistência (decidido)

1. Toy Pad **físico** ligado → tem prioridade: o remote não arranca (ou arranca em modo leitura, com
   aviso no log). O físico manda sempre.
2. App LegoToypad a correr → **convivem** (8090 HTTP vs 9191 TCP), porque ambos passam por
   `InjectToypadPacket`.
3. **Condição obrigatória da convivência:** qualquer mudança no portal — venha do remote, do app
   LegoToypad, do `portal.json`, do Toy Pad físico ou de um atalho no comando — tem de gerar um evento
   SSE. Sem isso o telemóvel fica desatualizado quando a app mexe no portal.

Implementação: um **único ponto de notificação** no adaptador. Toda a escrita no pad passa por uma
função `NotifyPortalChanged()` que incrementa a versão SSE (`s_version++` + `notify_all`). Os caminhos
a atingir no port: `LoadTag`, `RemoveIndex`, `MoveIndex`, `PlaceFigure`, o *watcher* do `portal.json`,
o listener 9191 e o Toy Pad físico. No Cemu: `LoadFigure`, `RemoveFigure`, `MoveFigure`, `TempRemove`,
`CancelRemove`, `SwapFigures`.
4. `portal.json` → passa a **espelho** quando o remote está ligado (o remote escreve o mesmo ficheiro),
   para não haver duas fontes de verdade a competir no *watcher* de 500 ms.

---

## 6. Otimizações a fazer na versão nova (defeitos lidos no código atual)

| # | Problema | Onde | Correção |
|---|---|---|---|
| 1 | `s_cv.notify_all()` + `StateJson()` completo por cada comando de LED; com vários telemóveis ligados e o jogo a animar zonas, são dezenas de JSON/s | `ToypadRemote.cpp:673-691` (SSE) + `:58` (`notify_all`) | coalescer: enviar no máximo a cada 50 ms e só quando `v` mudou desde o último envio **daquele** cliente |
| 2 | `ReadFavorites()` lê o disco em cada `GET /api/figures` | `:698` | cache em memória invalidada só nas escritas |
| 3 | `StateJson()` corre **dentro** de `s_mutex` e chama `GetSlots()`, que tranca `m_dimensionsMutex` → formatação JSON (e alocação) pagas com os dois locks na mão | `:161-200` | copiar o estado para valores simples com os locks, **largar os dois**, e só depois formatar. Ver ordem de locks na §4.1 — hoje não há inversão; o que existe é fragilidade e tempo de lock desnecessário |
| 4 | `s_slotFile` só em memória → depois de reiniciar, o pad mostra figuras **sem nome** | `:48` | o port já persiste o caminho na tag; usar isso como fonte |
| 5 | Sem `gzip`: `index.html` 24 KB + `catalog.js` 33 KB por arranque a frio no telemóvel | estáticos | `set_pre_compressed_file_handler` / compressão na build do web app |
| 6 | 17 MB de PNG carregados de uma vez na biblioteca | `img/` | lazy-load por `IntersectionObserver` + WebP (≈70 % menos bytes); pasta opcional do utilizador (§3.1) |
| 7 | Sem autenticação nem verificação de `Origin` → qualquer página aberta no PC pode `POST /api/place` (CSRF) | todas as rotas POST | **cabeçalho próprio obrigatório (`X-TPR: 1`) + verificação de `Origin`** em todos os POST; PIN opcional. Ver §4.2 |
| 8 | `bind 0.0.0.0` em rede pública | `:944` | manter, mas mostrar o URL só para adaptadores privados + Tailscale (já há deteção de `100.64/10`) |
| 9 | Log por comando a `LogType::Force` | `:956` | nível normal; debug atrás de flag |
| 10 | `s_selected` (atalhos) não sobrevive a reinícios nem é por jogador | `:50` | por jogador (1-4) quando se ligar o co-op |

---

## 7. Plano por fases

| Fase | Entrega | Ficheiros | Verificação |
|---|---|---|---|
| **F0** ✅ | Núcleo em repo próprio + `VERSION.txt` + `sync.py`, interface de host, site **embutido**, compila nos dois lados | `toypad-remote-core/*`, `web_embed.py`, adaptador Cemu | **feito**: 37 testes verdes no núcleo; `Cemu_release.exe` responde 200 em `/api/state` |
| **F1** ✅ | Port: espelho só de leitura + SSE coalescido + `NotifyPortalChanged()` único | `hle_toypad.h/.cpp` (`Slots`/`Leds`/callbacks), `hle_toypadremote.cpp` | **feito**: estado e LEDs no telemóvel; callbacks disparam fora do `g_mutex` |
| **F2** ✅ | Port: escrita (place/remove/move/zone/create) com o intervalo de 500 ms | adaptador usa `LoadTag`/`RemoveIndex`/`MoveIndex`/`SwapFigures` + `InjectToypadPacket` | **feito**: `POST /api/create` e `/api/remove` a 200 com o evento entregue ao jogo |
| **F3** | **Launcher: toggle + auto-start + `launcher.ini` + aviso "visível na rede local" + coexistência** | `tools/dist/launcher.cpp`, `main.cpp` | liga o toggle, arranca o jogo, servidor no ar — **sem isto ninguém usa o resto** |
| **F4** | Biblioteca de figuras (listar, favoritos, renomear, apagar, criar) com a **tabela de melhorias** (§3.1) | `CreateFigure`/`CreateVehicle` novos, tabela de upgrades | criar veículo, pôr no pad, sem "Please update…" |
| **F5** | Loadouts (guardar, aplicar por botão, A/B/X/Y) | núcleo | loadout repõe o pad |
| **F6** | Atalhos no comando (Home/PS + setas) | `hle_pad.cpp` (hook antes de entregar o `Input`) | botões escondidos do jogo, figura move |
| **F7** | Endurecimento + otimizações da §6 (o que sobrou: gzip, imagens lazy, cache) | núcleo | 4 telemóveis ligados, CPU < 1 % |
| **F8** | Sincronizar o núcleo de volta ao fork de Cemu | `ToypadRemote.cpp` → casca do adaptador | paridade funcional |

Ordem de risco crescente. **F1+F2+F3** é o mínimo utilizável (controlar o portal do telemóvel, arrancando
sozinho com o jogo). A junção SSE entra em F1 porque é barata e evita carga desnecessária desde o início.

---

## 8. Riscos e armadilhas

1. **Duas fontes de verdade no pad.** `portal.json` + `StartPortalWatcher` (500 ms) + remote a escrever
   pode criar figuras fantasma. Resolver antes de F2 (§5.1).
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

## 9. Decisões tomadas

| # | Decisão |
|---|---|
| 1 | Núcleo em **repo próprio**, copiado por script de sincronização (ou `git subtree`), commit fixado em `VERSION`. **Sem submodule.** |
| 2 | **`cpp-httplib`** (MIT, header-only), isolado dentro do núcleo. |
| 3 | **`X-TPR: 1` + verificação de `Origin` sempre obrigatórios.** PIN opcional, desligado por omissão (Tailscale dispensa). |
| 4 | Remote e app LegoToypad **convivem**; toda a mudança no portal gera evento SSE. Toy Pad físico tem prioridade. |
| 5 | **ID + nome**, com papéis diferentes (§3.2). |
| 6 | **Site embutido no executável**; `resources/` só como alternativa de desenvolvimento. |
| 7 | Sem `.bin` do harrysof nem imagens da wiki no projeto: tabela de melhorias + geração de tags; imagens por script do utilizador (§3.1). |

### Ainda em aberto

1. Botão modificador dos atalhos no port (proposta: `kTouchPad`; alternativa `Options` + `L3`).
2. Formato da tabela de melhorias (C++ compilado vs. TSV lido ao arranque — o TSV facilita correções
   pela comunidade sem recompilar).
3. Nome do repo do núcleo e da licença (MIT para alinhar com `httplib`, ou GPL-2.0 para alinhar com o port).
