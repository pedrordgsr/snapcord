# Snapcord — Definição do Projeto

> Este documento é a fonte de verdade do projeto. Todas as decisões abaixo já foram
> discutidas e fechadas com o dono do projeto — não reabra essas decisões sem motivo forte.

## Contexto para o Claude

- O dono do projeto **não programa e não quer mexer em código**. O Claude faz todo o
  trabalho técnico; o usuário decide, testa e aprova.
- Comunique-se **em português**, em linguagem simples, explicando o que foi feito e
  como testar.
- **Peça confirmação antes de** instalações grandes (SDKs, toolchains), ações no GitHub
  (criar repositório, push, releases) ou qualquer coisa difícil de desfazer.
- Trabalhe em passos pequenos e verificáveis: cada etapa deve terminar com algo que o
  usuário consiga rodar e ver funcionando.
- **Antes de abrir ou atualizar um pull request**, rode/confira o CI do repositório
  (GitHub Actions: `build` em Windows, Linux, macOS e Flatpak). Não envie o PR (nem diga
  que está pronto) enquanto os jobs relevantes não estiverem verdes — ou, se ainda
  estiverem rodando, acompanhe até o fim e corrija falhas antes de considerar a tarefa
  concluída. Empurrar um commit de correção e esperar o CI revalidar faz parte do fluxo.
- **Nunca** registre tokens, tickets de login ou dados de conta em logs, commits ou
  mensagens.

## Visão

Cliente de Discord **nativo, leve e de código aberto**, com **foco principal em chamadas
de voz** (mais importante que chat ou navegação de servidores). Layout fiel ao Discord
oficial, consumo mínimo de CPU e RAM, experiência completa no estilo do Ripcord.
Será distribuído publicamente.

## Decisões fechadas

| Item | Decisão |
|---|---|
| Nome | **Snapcord** (definitivo) |
| Licença | **GPLv3** |
| Hospedagem | GitHub, na conta do dono do projeto |
| Plataformas | **Windows, Linux e macOS desde o início**. Desenvolvimento principal no Windows, mas CI compila as três a cada mudança |
| Foco | **Voz** acima de tudo |
| Vídeo / compartilhamento de tela | **Fora do escopo** (não é prioridade) |
| Contas | Uma conta por vez |
| Login | **QR code** (protocolo Remote Auth, escaneado pelo app do celular) |
| Idioma do app | **Inglês por padrão**, com opção de **português (Brasil)** no menu de configurações |
| Idioma do código | README, comentários, nomes e scripts **em inglês** (projeto aberto internacional). A conversa com o dono do projeto continua em português |
| Visual | Layout **fiel ao Discord** (barra de servidores, lista de canais, painel de voz/usuário embaixo, área central, lista de membros). Tema escuro. Sem efeitos pesados ("eye candy") |

## Stack

| Área | Escolha | Observações |
|---|---|---|
| Linguagem | C++20 | A libdave (E2EE oficial do Discord) é C++ |
| Build / dependências | CMake + vcpkg (com CMakePresets) | Mesmo fluxo nas três plataformas |
| Interface | Qt 6 Widgets + QSS | Sem QML, sem WebView. Redesenha só o que muda |
| Rede | Qt Network + Qt WebSockets | Gateway, REST, voice gateway |
| UDP de voz | QUdpSocket ou socket nativo | Em thread própria |
| Captura/reprodução de áudio | **miniaudio** | WASAPI / CoreAudio / PipeWire-PulseAudio-ALSA |
| Codec | **Opus** (libopus) | 48 kHz, frames de 20 ms, PLC e FEC |
| Criptografia de transporte | `aead_aes256_gcm_rtpsize` / `aead_xchacha20_poly1305_rtpsize` | libsodium e/ou OpenSSL |
| E2EE | **libdave** (protocolo DAVE / MLS) | Obrigatório nas chamadas de voz do Discord |
| Supressão de ruído | RNNoise | Fase 3 |
| Cancelamento de eco | WebRTC AudioProcessing (opcional) | Fase 3 |
| Credenciais | QtKeychain | Credential Manager / Keychain / libsecret |
| Fonte | Fonte aberta parecida (Noto Sans ou Inter) | A fonte oficial (gg sans) é proprietária |

**Traduções:**
- Todo texto visível da interface passa por `tr()` e é escrito em inglês.
- A tradução fica em `translations/snapcord_pt_BR.ts` (formato Qt Linguist) e entra no app via `qt_add_translations`.
- Ao adicionar ou alterar um texto da interface, atualize também esse arquivo.
- O idioma escolhido fica salvo no QSettings (chave `language`) e vale ao reiniciar o app.

**Restrições:** não usar logo, marca nem a fonte proprietária do Discord. Qt é LGPL,
então o link tem que ser dinâmico.

## Metas de desempenho

- RAM: ~40–90 MB (sem e com chamada)
- CPU parado: ~0% (tudo orientado a eventos, sem polling)
- CPU em chamada: poucos % de um núcleo (Opus + RNNoise)
- Abrir em menos de 1 segundo
- Latência de voz equivalente à do cliente oficial

## Arquitetura

- **`core`** (sem interface): sessão, gateway, REST, autenticação, cache e modelos de dados.
- **`voice`**: motor de voz isolado em threads próprias. A thread de áudio em tempo
  real nunca bloqueia nem espera pela interface. Inclui:
  - voice gateway
  - UDP, descoberta de IP e RTP
  - criptografia e DAVE
  - Opus
  - jitter buffer
  - mixagem com volume por usuário
  - VAD e push-to-talk
- **`platform`**: tudo que depende do sistema operacional, atrás de interfaces:
  - hotkey global de push-to-talk
  - keychain
  - notificações
  - inicialização com o sistema
- **`app`**: interface Qt Widgets. Só desenha e repassa ações para o `core`/`voice`.
- **Cache enxuto:**
  - Guardar só o necessário em memória, com LRU de canais.
  - Membros carregados sob demanda.
  - Imagens decodificadas já no tamanho de exibição.
  - Animações pausadas quando fora da tela.
- **Gateway:**
  - Compressão `zlib-stream`.
  - Descartar cedo os eventos que nenhuma tela usa.
  - Identificação na conexão (IDENTIFY) e ritmo de requisições imitando fielmente o cliente oficial, para reduzir o risco de banimento.
- **Regra anti-abuso (decisão do dono):** toda requisição deve ser feita **do mesmo jeito que o cliente oficial faz**.
  Nada de caminhos alternativos ou "fallbacks" que o app oficial não usa (ex.: upload por multipart); se o caminho
  oficial falhar, mostre o erro ao usuário em vez de tentar outro.

### Estrutura de pastas proposta

```
snapcord/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── LICENSE                  (GPLv3)
├── README.md                (com aviso sobre os Termos de Serviço)
├── src/
│   ├── app/                 (main, janelas, widgets, QSS)
│   ├── core/                (auth, gateway, rest, cache, models)
│   ├── voice/               (voicegateway, udp, crypto, dave, opus, audio, mixer)
│   └── platform/            (win/, mac/, linux/)
├── resources/               (qss, ícones, fontes, sons)
├── tests/
└── .github/workflows/       (build Windows / Linux / macOS)
```

## Fases

### Fase 0: Ambiente e repositório
- Instalar Qt 6 (LTS), CMake, Ninja e vcpkg.
- Criar a estrutura, o LICENSE e o README.
- CI no GitHub Actions compilando para Windows, Linux e macOS.
- Uma janela vazia abrindo nas três plataformas.

### Fase 1: Núcleo de voz (prioridade máxima)
- Login por QR code, com o token guardado no keychain.
- Conexão ao gateway e lista de servidores e canais de voz.
- Entrar e sair de canal de voz, falar e ouvir, com **DAVE funcionando**.
- Mutar e ensurdecer.
- Indicador de quem está falando.
- Volume por usuário.
- Escolha de microfone e saída.
- Detecção de voz (VAD) com ajuste de sensibilidade, e push-to-talk.

### Fase 2: Qualidade de voz
- Supressão de ruído (RNNoise) e cancelamento de eco.
- Ajuste automático de ganho.
- Sons de entrar, sair, mutar e desmutar.
- Chamadas em DMs e grupos.
- Painel de conexão (ping, perda de pacotes).

### Fase 3: Chat
- Canais de texto e DMs.
- Markdown do Discord, menções, emojis e imagens.
- Mensagens não lidas e notificações.
- Respostas, reações, edição e exclusão.

### Fase 4: Lançamento nas três plataformas
- Instaladores:
  - Windows: `.exe`/MSI
  - macOS: `.dmg`
  - Linux: AppImage e Flatpak
- Atualização automática.
- Push-to-talk global em cada SO.
- Testes de áudio:
  - Linux: PipeWire e PulseAudio
  - macOS: permissão de microfone
- Releases no GitHub.

> Mesmo com o lançamento por último, o CI continua compilando Windows, Linux e macOS
> desde a Fase 0, para que nada específico de um SO entre escondido no código.

## Notas técnicas de referência

> Os protocolos do Discord não são documentados para clientes de usuário e mudam sem
> aviso. **Confira sempre em fontes atualizadas** antes de implementar: Discord
> Developer Docs (gateway e voz), o repositório discord/libdave, discord-userdoccers e
> clientes abertos como Abaddon e Discordo.

**Login por QR (Remote Auth):**
1. Conectar em `wss://remote-auth-gateway.discord.gg/?v=2` com o header `Origin: https://discord.com`
   (no Qt, o origin precisa ir no construtor do `QWebSocket`; header manual é ignorado e o Discord responde 403).
2. Gerar um par RSA-2048 e enviar a chave pública. O servidor responde com um nonce criptografado.
3. Descriptografar o nonce (RSA-OAEP/SHA-256) e enviar o nonce decifrado em base64url (não o hash).
4. Receber o `fingerprint` e mostrar o QR com `https://discord.com/ra/<fingerprint>`.
5. Após o scan, mostrar a prévia do usuário.
6. Após a confirmação no celular, receber o ticket.
7. Fazer `POST /users/@me/remote-auth/login` com o ticket e descriptografar o token recebido.

Pode aparecer captcha. É preciso tratar esse caso e ter um fallback.

**Voz:**
1. Pelo gateway principal (op 4, Voice State Update), receber `VOICE_STATE_UPDATE` e `VOICE_SERVER_UPDATE`.
2. Conectar ao voice gateway (v8) e fazer a descoberta de IP via UDP.
3. Enviar `Select Protocol` com o modo `*_rtpsize`.
4. Receber a chave de sessão.
5. Enviar RTP com Opus.
6. Aplicar a camada DAVE (opcodes próprios no voice gateway) por cima.

**Riscos conhecidos:**
- **Termos de Serviço:** clientes de terceiros violam os Termos do Discord e podem
  causar banimento. O README deve avisar isso claramente.
- **Manutenção:** o protocolo de voz e o DAVE podem mudar e exigir atualizações.
- **Push-to-talk global:**
  - Windows: tranquilo.
  - macOS: exige permissão de acessibilidade.
  - Linux com Wayland: depende do portal *GlobalShortcuts*; precisa de fallback.
- **Assinatura de código:**
  - Windows: sem certificado, o SmartScreen mostra aviso.
  - macOS: sem a conta de desenvolvedor da Apple (US$ 99/ano), o usuário precisa
    liberar o app manualmente.
  - É possível lançar sem assinar no início.

## Estado do ambiente (Windows)

| Ferramenta | Onde |
|---|---|
| Visual Studio 2022 Build Tools (MSVC) | Instalado no sistema |
| Qt 6.8.3 (MSVC 64-bit, com WebSockets e ImageFormats) | `C:\Users\Pedro\Qt\6.8.3\msvc2022_64` (via `aqtinstall`) |
| CMake e Ninja | Via pip, em `%APPDATA%\Python\Python314\Scripts` |
| vcpkg | `C:\Users\Pedro\vcpkg` (o baseline do `vcpkg.json` é o commit desse clone) |

- **Compilar:** `scripts\build.ps1` (Debug), `-Config release`, `-Run` para abrir.
  - O script entra no ambiente do MSVC e força o nosso vcpkg: o Developer Shell do VS troca
    `VCPKG_ROOT` pelo vcpkg embutido dele, que é antigo e quebra o build.
- **Executável:** `build\<config>\Snapcord.exe`.
- **Testes:** `build\<config>\snapcord_tests.exe` (Qt Test).
  - É um app de janela no Windows, então use `-o arquivo.txt,txt` para ver o relatório.
- **Log do app:** `%LOCALAPPDATA%\Snapcord\Snapcord\snapcord.log`.
  - A execução anterior fica em `snapcord.old.log`.
  - Nunca registrar tokens nem chaves.
- **Traduções:** depois de mudar textos, rode o alvo `Snapcord_lupdate` e confira se não
  sobrou `type="unfinished"` no `.ts`.

## Estado do projeto

- **Fase 0:** concluída.
- **Fase 1:** concluída e **testada com uma conta real**. O log da chamada confirmou:
  - DAVE v1 funcionando, com welcome aceito;
  - áudio indo e voltando;
  - zero falhas de decodificação.
  - As "E2EE encrypt failures" enquanto o usuário está sozinho no canal são normais: o grupo
    MLS só se forma quando chega o segundo participante.
- **Fase 2:** código completo, compila sem avisos e 15 testes passando. **Falta o teste real**
  das chamadas em DM, dos sons e do processamento de voz com o microfone.
  - **Processamento de voz (`AudioProcessor`):** pipeline em blocos de 10 ms, nesta ordem:
    1. Ganho de entrada.
    2. Cancelamento de eco (SpeexDSP MDF, cauda de 150 ms). A referência vem do áudio de
       saída, via `EchoReference`.
    3. Supressão de ruído RNNoise 0.2, compilado do tarball em `cmake/Dependencies.cmake`,
       com SSE4.1/AVX2 escolhidos em tempo de execução.
    4. Controle automático de ganho (preprocessador do SpeexDSP).
  - **Sensibilidade automática:** usa a probabilidade de voz do RNNoise (≥ 0,6) mais um piso
    de -60 dB.
  - **Sons (`SoundEffects`):** sintetizados em código, sem arquivos e sem os sons do Discord.
    Tocam num stream de saída próprio, aberto só enquanto há som tocando.
    - Página **Configurações > Sound Effects**: cada som (entrar/sair, alguém entra/sai, mutar, ensurdecer,
      alguém muta/desmuta, toque, mensagem) tem um estilo (`Classic`, `Soft`, `Digital`, `Pop` ou `Off`), com botão
      de prévia. Só o estilo escolhido fica em memória; salvo no QSettings em `sounds/<som>`.
    - Opção **"tocar um som quando outros mutarem/desmutarem"** (`voice/participantMuteSounds`, desligada por
      padrão): `VoiceController::updateParticipants` compara `selfMute || mute` de quem continua no canal.
  - **Chamadas em DM e grupos:**
    - Capability `AUTO_CALL_CONNECT`; eventos `CALL_CREATE`, `CALL_UPDATE` e `CALL_DELETE`.
    - `VOICE_STATE_UPDATE` sem `guild_id`; o `server_id` da voz é o ID do canal.
    - Ao iniciar uma chamada nova, o cliente toca para os outros com
      `POST /channels/{id}/call/ring`; recusar usa `.../call/stop-ringing`.
  - **Painel de conexão:** clicar em "Voz conectada" mostra o gráfico de ping, a perda de
    pacotes recebidos (frames escondidos pelo PLC) e o estado do DAVE.

### Arquitetura implementada

- **`src/core`**
  - `RemoteAuth`: login por QR.
  - `Gateway`: zlib-stream, heartbeat, resume, backoff.
  - `Session`: READY, guilds, canais, permissões e voice states.
  - `RestClient`, `ClientProperties`, `Log`.
- **`src/voice`**
  - `VoiceConnection`: orquestra a chamada.
  - `VoiceGateway`: v8, com DAVE binário.
  - `DaveSession`: port do `DaveSessionManager.ts` da libdave.
  - `TransportCipher`: AES-GCM via OpenSSL e XChaCha via libsodium.
  - `UdpSocket`: sockets nativos e IP discovery.
  - `JitterBuffer`, `OpusCodec`, `AudioEngine` (miniaudio), `VoiceSettings`.
- **`src/platform`**
  - `CredentialStore`: Windows Credential Manager; stub nos outros sistemas.
  - `KeyState`: `GetAsyncKeyState` para o push-to-talk; stub nos outros sistemas.
- **`src/app`**
  - Telas: `LoginWindow`, `MainWindow`, `SettingsDialog`.
  - Componentes: `ServerRail`, `ChannelSidebar` (com delegate próprio), `VoiceChannelView`,
    `VoicePanel`, `UserPanel`.
  - Lógica: `VoiceController` (entrar, sair, mutar, ensurdecer), `ImageCache` (CDN com cache
    em disco), `AppController` (troca entre login e janela principal).
- **Threads da voz:**
  - O áudio do microfone é codificado e enviado direto no callback de captura.
  - Uma thread dedicada recebe o UDP e preenche jitter buffers por SSRC.
  - O callback de saída decodifica e mixa.
  - A sinalização roda na thread principal.
- **libdave:** entra via `FetchContent`, fixada no commit `8de72b1f`. A `mlspp` vem de um
  overlay port em `vcpkg/ports/mlspp`.

### Manutenção conhecida

- **Identificação do cliente:** `ClientProperties.cpp` imita o **cliente web no Chrome** (decisão do dono,
  seguindo o Acheron), não o app de desktop.
  - Campos na mesma ordem do cliente web (`core/OrderedJson`, porque o `QJsonObject` ordena as chaves), incluindo
    `launch_signature` (UUID com os bits de "client mod" zerados), `client_app_state` (focused/unfocused),
    `client_heartbeat_session_id` (renovado em uso, expira após 30 min parado, guardado no QSettings) e, só no
    IDENTIFY, `is_fast_connect` e `gateway_connect_reasons`.
  - Gateway: heartbeat pelo opcode 40 (QoS, com `foregrounded`/`rtc_connected`) e opcode 41 (sessão de heartbeat)
    após o READY e quando uma sessão nova começa. Foco da janela e chamada vêm do `AppController`.
  - Headers da API: `X-Super-Properties`, `X-Discord-Locale` (idioma da conta), `X-Discord-Timezone`,
    `X-Debug-Options`, `Referer` e `Origin` (só em métodos que alteram algo). WebSockets com `Origin: https://discord.com`.
  - O **build number** é lido do site (`discord.com/app` → script `sentry`), guardado por 1 dia. A chave
    `discord/clientBuildNumber` do QSettings força um valor, se a busca parar de funcionar.
  - A versão do Chrome (`ChromeVersion`) é fixa e precisa ser atualizada de tempos em tempos.
  - **Fora de escopo (decisão fechada):** imitar a assinatura TLS/HTTP2 do Chrome (curl-impersonate). A rede
    continua sendo a do Qt.
- **Disciplina de requisições (para não parecer abuso):** o app deve se comportar como o cliente oficial e
  nunca martelar a API. Regra geral ao mexer no código: antes de adicionar uma requisição nova, confira se já
  existe cache, debounce ou batch para aquilo, e **respeite sempre o rate limit**.
  - **Rate limit (429):** `RestClient` relê a resposta 429, espera o `retry_after` (campo do corpo, ou header
    `Retry-After`) e repete — no máximo 3 vezes, com teto de 60 s. Nunca repetir um 429 na hora.
  - **Debounce/batch já existentes (não remover):** presença (op 3) agrupada em 1 s; `typing` no máximo a cada
    8 s; usuários desconhecidos pedidos em lote de até 100 a cada 250 ms (op 8); inscrição da lista de membros
    (op 37) adiada 150 ms e ignorada se as faixas não mudaram; read ack só quando o último lido muda ou há
    menção a limpar.
  - **Cache (não encurtar à toa):** perfis por 3 min; build number por 1 dia; imagens em memória + disco
    (`PreferCache`); `MessageStore` com LRU de 8 canais.
  - **Backoff:** gateway e voz reconectam com espera exponencial; o login por QR espera antes de refazer.
  - **Rich presence (jogos e Spotify): DESLIGADO** por decisão do dono (`RichPresence::Enabled == false`).
    Com isso não há varredura de processos (15 s), download da lista `detectable` (~13 MB) nem polling do
    Spotify (15 s). Para religar, mudar a constante e recompilar; a página "Activity Privacy" reaparece sozinha.
- **Captcha no login por QR:** não é suportado e acontece na prática.
  - A alternativa é o **login por token**, na própria tela de login ("Log in with a token instead").
  - O token é validado com `GET /users/@me` antes de ser salvo.

### Fase 3 (chat)

- **Dados:**
  - `core/Message` é o modelo de mensagem.
  - `core/MessageStore` guarda no máximo 8 canais em memória (LRU) e carrega 50 mensagens por
    vez, mais o histórico sob demanda.
  - Mensagens enviadas aparecem na hora como "pendentes" e são trocadas pela versão do servidor
    quando o eco chega pelo gateway (casamento pelo `nonce`).
- **Markdown:** `core/Markdown` converte o markdown do Discord para o HTML que o QTextDocument
  entende. Fragmentos protegidos por marcadores Unicode de uso privado evitam formatar dentro de
  código, links e menções. Tem testes em `tests/MarkdownTest.cpp`.
- **Não lidas:** read states do READY, comparando o `last_message_id` do canal com o ID lido.
  - Ack com `POST /channels/{c}/messages/{m}/ack`, também pelo evento `MESSAGE_ACK`.
  - Menções contadas localmente.
  - `user_guild_settings` define o que está silenciado.
- **Interface:** `app/MessageView` traz o modelo, o delegate (layout cacheado por mensagem e
  largura) e a lista com hit-test. `app/ChatView` traz o cabeçalho, a lista e o compositor.
  Ainda há `EmojiPicker` e `Notifier` (bandeja do sistema mais o som `Message`).
- **Diagnóstico do microfone:** o log de voz traz quadros de voz enviados, quadros mutados, pico
  de nível e pico de probabilidade de voz, para investigar quando "ninguém me ouve".
- **Build com o app aberto:** `scripts\build.ps1 -Target <alvo>` compila só um alvo, útil quando o
  `Snapcord.exe` está aberto e não pode ser sobrescrito.

### Anexos e menções no compositor

- **Anexos:** botão "+", arrastar e soltar, ou colar (arquivos copiados ou imagem da área de transferência, que vira
  `image.png`). Ficam numa bandeja (`app/AttachmentTray`) acima do texto até o envio.
- **Envio:** como o cliente oficial, `POST /channels/{id}/attachments` devolve um `upload_url` por arquivo; o arquivo
  vai por `PUT` direto para lá e a mensagem cita o `uploaded_filename`. **Sem alternativa:** se falhar, a mensagem
  fica como "falhou" (nada de `multipart/form-data`). A mensagem pendente mostra o progresso em cada arquivo.
- **Limites** (`core/UploadLimits.h`): 10 arquivos por mensagem, 500 MiB no total; por arquivo, 10 MiB grátis,
  50 MiB Nitro Basic/Classic, 500 MiB Nitro, ou 50/100 MiB em servidores de nível 2/3 (vale o maior). Texto: 2000
  caracteres (4000 com Nitro). Permissão `ATTACH_FILES` esconde o "+" quando falta.
- **Menções:** digitar `@` ou `#` abre `app/MentionPopup` (membros, cargos mencionáveis, `@everyone`/`@here` com
  permissão, canais). O texto mostra `@Nome`; no envio, `core/Mentions` troca por `<@id>`, `<@&id>` e `<#id>` (e faz o
  inverso ao editar). Membros ainda desconhecidos são buscados pelo opcode 8 com `query`. Testes em
  `tests/ComposerTest.cpp`.
- **Falta o teste real** com uma conta (principalmente o upload pelo `upload_url`).

### Lista de membros

- **Protocolo:** o cliente se inscreve com o opcode 37 (Guild Subscriptions Bulk), pedindo faixas de 100 linhas
  do canal aberto (sempre `[0,99]`, mais a faixa visível ao rolar). O servidor responde com
  `GUILD_MEMBER_LIST_UPDATE` (ops `SYNC`, `INSERT`, `UPDATE`, `DELETE`, `INVALIDATE`), agrupando por cargos
  separados, "online" e "offline".
- **Dados:** `Session` acompanha só a lista do canal na tela (`subscribeMemberList`, `memberList`). Canais vistos
  pelas mesmas pessoas compartilham uma lista; o ID é aprendido no primeiro `SYNC` (com um palpite por murmur3
  dos overwrites de `VIEW_CHANNEL` para reconhecer a lista já carregada). Testes em `tests/MemberListTest.cpp`.
- **Interface:** `app/MemberListView` (240 px, à direita do chat), com botão no cabeçalho para mostrar/esconder
  (QSettings `ui/memberList`). Clique abre o perfil à esquerda; botão direito abre o menu do usuário.
- **Falta o teste real** com uma conta (principalmente servidores grandes e a rolagem).

### Perfis e presença

- **Presença:** `Session` guarda status e atividades vindos de `READY_SUPPLEMENTAL` (`merged_presences`),
  `PRESENCE_UPDATE`, `GUILD_MEMBERS_CHUNK` e `GUILD_CREATE`. Ao abrir o perfil de um membro de servidor sem
  presença conhecida, pede pelo opcode 8 com `presences: true`.
- **Própria presença:** status e status personalizado vêm do `user_settings` do READY; as atividades das outras
  sessões vêm de `SESSIONS_REPLACE` (sessão `all`). Depois do READY o app envia opcode 3 com o status real e o
  status personalizado.
- **Perfil:** `GET /users/{id}/profile` (com `guild_id` dentro de servidores, para cargos e data de entrada),
  em cache por 3 minutos. Interface em `app/ProfileCard` (cartão), `app/ProfilePopup` (popout) e
  `app/ProfileEditor` (editar perfil e status personalizado).
- **Abrir perfil:** clicando no avatar/nome de uma mensagem, numa @menção (links `user:<id>`), num membro em
  canal de voz, no bloco da chamada, no menu de contexto ("Profile") e no próprio nome no painel do usuário.
- **Edição:** `PATCH /users/@me` (nome exibido e avatar) e `PATCH /users/@me/profile` (pronomes, bio e cor do
  banner). Status e status personalizado usam o endpoint legado `PATCH /users/@me/settings` (documentado como
  depreciado, mas funcional) mais o opcode 3. Banner com imagem e temas de perfil exigem Nitro: ficam fora.
- **Falta o teste real** com uma conta.

### Rich presence (jogo e Spotify)

> **DESLIGADO** no momento (`RichPresence::Enabled == false`). O código abaixo continua existindo, mas não roda:
> nada é detectado nem compartilhado e a página "Activity Privacy" fica escondida. Ver "Disciplina de requisições".

- **Jogos:** `core/GameDetector` baixa a lista pública `GET /applications/detectable` (~13 MB), guarda uma versão
  compacta em `detectable.tsv` (pasta de dados do app, renovada a cada 3 dias) e compara com os processos abertos a
  cada 15 s (`platform/ProcessList`). Regras da lista: o nome pode ser o fim de um caminho (`_retail_/wow.exe`);
  `>nome` exige nome exato mais os `arguments` na linha de comando; launchers são ignorados. No Linux, jogos do
  Proton/Wine são achados pelo primeiro argumento `.exe`. Atividade enviada: tipo 0 com `application_id`.
- **Spotify:** `core/SpotifyPresence` usa a conexão do Spotify na conta (`GET /users/@me/connections`, respeitando
  `show_activity`), pega o token em `.../connections/spotify/{id}/access-token`, abre o `wss://dealer.spotify.com`
  e assina `PUT /v1/me/notifications/player?connection_id=...`. Se a assinatura falhar, consulta `/v1/me/player`
  a cada 15 s. Atividade tipo 2 "Spotify", flags 48, `sync_id`, `party.id = spotify:<user>`, capa `spotify:<id>`.
- **Envio:** `Session::setLocalActivity` junta jogo e música ao status personalizado no opcode 3 (agrupado em 1 s,
  e só depois do READY). Interruptores em Configurações > Activity Privacy (`activity/shareGames`,
  `activity/shareSpotify`), ligados em `app/RichPresence`.
- **Limitação:** no Flatpak o sandbox não enxerga os processos do sistema, então jogos não são detectados lá.
- **Falta o teste real** com uma conta.

### Pastas de servidores e convites

- **Pastas:** `core/GuildFolders` guarda o modelo (configuração `guild_folders` da conta) e as operações de
  arrastar (`moveGuild`, `moveFolder`), com testes em `tests/GuildFoldersTest.cpp`. Servidores novos aparecem no
  topo. Salvar usa `PATCH /users/@me/settings` com `guild_folders` (mesmo endpoint legado do status; o cliente
  oficial usa o `settings-proto`). Mudanças vindas de outros clientes chegam por `USER_SETTINGS_UPDATE`.
- **Barra (`app/ServerRail`):** arrastar reordena; soltar um servidor no meio de outro cria pasta; no meio de uma
  pasta fechada, coloca dentro. Pastas abertas ficam no QSettings (`ui/openFolders`, só local, como no Discord).
  Botão direito: "Convidar pessoas" e "Sair do servidor" (dono não vê); na pasta, "Configurações da pasta"
  (nome e cor, `app/ServerDialogs`). Botão "+" abre "Entrar em um servidor".
- **Convites:** `GET /invites/{code}?with_counts=true&with_expiration=true` (com `inputValue` quando digitado),
  em cache por 5 min, inclusive falhas. Entrar: `POST /invites/{code}` com `session_id` e o header
  `X-Context-Properties` ("Join Guild" no diálogo, "Invite Button Embed" no cartão do chat). Criar:
  `POST /channels/{id}/invites` (7 dias, sem limite de usos), exige `CREATE_INSTANT_INVITE`. "Convidar" para uma DM
  manda o link como mensagem normal, como o cliente oficial. Sair: `DELETE /users/@me/guilds/{id}`.
- **Chat:** links `discord.gg/...` e `discord.com/invite/...` ganham um cartão com o servidor e o botão "Entrar"
  (`MessageDelegate::paintInvite`); clicar no link abre o diálogo de convite dentro do app.
- **Captcha:** entrar em servidor pode pedir captcha; o app mostra o erro (sem alternativa).
- **Falta o teste real** com uma conta.

### Gerenciar canais

- **Permissão:** `MANAGE_CHANNELS` (`Session::canManageChannels`): no servidor para criar fora de categoria, na
  categoria para criar dentro dela, no próprio canal para editar/excluir. Sem permissão, as opções não aparecem.
- **Criar:** `POST /guilds/{id}/channels` com `type`, `name`, `permission_overwrites: []` e `parent_id` (se em
  categoria). Aberto pelo "+" que aparece ao passar o mouse numa categoria, pelo botão direito na lista de canais
  (ou no espaço vazio) e pelo menu do servidor na barra. Canal de texto novo abre na hora.
- **Editar:** `PATCH /channels/{id}` só com os campos alterados (`app/ChannelSettingsDialog`): nome; texto tem tópico,
  modo lento e restrição de idade; voz tem bitrate (máximo pelo nível de boost: 96/128/256/384 kbps) e limite de
  usuários. Categorias só têm nome. Nome de canal de texto vira minúsculo com hífens enquanto digita.
- **Excluir:** `DELETE /channels/{id}`, sempre com confirmação. Se o canal aberto some, o app vai para outro.
- A resposta da API já atualiza a `Session` (o evento do gateway chega depois e só confirma).
- **Falta o teste real** com uma conta.

### Animações

- **`app/Motion`** concentra tudo: transições curtas (120–180 ms) que só gastam CPU enquanto rodam; parado,
  continua ~0%.
  - `Motion::Value`: número que desliza até o alvo (pílula e forma dos ícones na `ServerRail`, borda verde do
    `ParticipantTile`). `setOnChange` permite redimensionar algo junto: as pastas da `ServerRail` abrem e fecham
    deslizando (caixa com os servidores muda de altura e recorta) e o mosaico de ícones vira o símbolo de pasta
    com fade.
  - `Motion::ItemAnimator`: hover/seleção/fala em delegates (`ChannelSidebar`, `MemberListView`), repintando só
    as linhas em movimento.
  - Popups, menus, tooltips e diálogos aparecem com fade (opacidade da janela, via filtro no app). Desligado no
    Wayland, que não suporta opacidade de janela. Os efeitos próprios do Qt (`UI_FadeMenu` etc.) ficam desligados.
  - Troca de páginas (Configurações, login) usa `crossFade` (foto da tela que some); a lista de membros desliza
    com `slidePanel`, sem relayout do chat a cada quadro.
- **Evitar:** `QGraphicsOpacityEffect` permanente, sombras e desfoque. Hover de mensagens no chat continua
  instantâneo, como no Discord.
- **"Reduce motion"** em Configurações > Aparência (QSettings `ui/reduceMotion`) desliga tudo. O modo
  `--screenshots` também desliga.

### Fase 4 (lançamento)

- **Fora de escopo por decisão do dono:** instalar atualizações sozinho, testes de áudio por plataforma e
  push-to-talk global no macOS/Linux.
- **Aviso de atualização (`core/UpdateChecker`, `app/UpdateDialog`):** só avisa, nunca baixa nem instala.
  - Consulta `api.github.com/repos/pedrordgsr/snapcord/releases/latest` (ou `/releases`, com "incluir versões de
    teste") no máximo 1 vez por dia, primeira checagem ~15 s após abrir. Última checagem em `updates/lastCheck`.
  - Achou versão maior (comparação semver, testes em `tests/UpdateCheckerTest.cpp`): mostra a janela com as notas
    da versão e os botões Baixar / Pular esta versão (`updates/skippedVersion`) / Depois. "Baixar" abre o pacote
    do sistema (`.exe`, `.dmg`, `.AppImage` ou `.flatpak`, pelo nome do arquivo) ou a página da versão.
  - Configurações > **About**: versão, links, aviso dos Termos, colaboradores (API de contributors do GitHub,
    buscada só ao abrir a página), "Procurar atualizações" e as opções `updates/automatic` e `updates/prereleases`.
- **Login guardado no sistema:** Windows Credential Manager, macOS Keychain e Linux Secret Service
  (libsecret), em `src/platform/CredentialStore*.cpp`.
- **Ícones:** gerados a partir do SVG pela ferramenta `snapcord_render_icons`
  (`tools/render_icons.cpp`, fora do build padrão). Os resultados ficam em `packaging/icons/` e vão
  para o repositório.
- **Metadados:**
  - `packaging/windows/snapcord.rc.in`: ícone e versão do `.exe`.
  - `packaging/macos/Info.plist.in`: inclui `NSMicrophoneUsageDescription`, obrigatório para o
    microfone funcionar no macOS.
  - `packaging/linux/*.desktop` e `*.metainfo.xml`.
  - ID do app: `io.github.pedrordgsr.Snapcord`.
- **Pacotes:**
  - Windows: `packaging/windows/package.ps1` gera o `.zip` portátil e o instalador Inno Setup
    (`snapcord.iss`).
  - macOS: `packaging/macos/package.sh` usa macdeployqt, assinatura ad-hoc e gera o `.dmg`.
  - Linux: `packaging/linux/package.sh` gera o AppImage; `package-deb.sh` gera o `.deb`
    (Qt embutido em `/opt/snapcord`). AppDir compartilhado via `prepare-appdir.sh`.
  - AUR: `packaging/aur/snapcord-bin/` (AppImage das Releases; publicação no AUR é manual).
  - Flatpak: manifesto em `packaging/flatpak/`, runtime KDE 6.8, `libsecret` compilado no
    manifesto (o SDK traz um `.pc` quebrado), demais dependências via vcpkg com rede
    liberada no build.
- **CI (`.github/workflows/build.yml`):**
  - Compila, testa e empacota nas três plataformas, mais o Flatpak.
  - Uma tag `v*` cria uma Release **rascunho** com todos os pacotes.
  - Quando falha, o passo "Report errors" publica as linhas de erro como anotações, que são
    públicas. Assim dá para ler pela API sem login:
    `GET /repos/pedrordgsr/snapcord/check-runs/{job_id}/annotations`.
- **Modo demonstração:** `Snapcord --demo` abre a interface real com dados inventados (`src/app/Demo.cpp`), sem
  login e sem rede. `--demo --screenshots <pasta>` salva as capturas usadas no README (`docs/screenshots/`) e fecha.
- **Verificado localmente:**
  - O `.zip` do Windows tem 15 MB, roda sem o Qt instalado e ocupa ~34 MB de RAM em Release.
  - Linux, macOS e Flatpak só podem ser verificados pelo CI.

## Próximo passo

1. O dono do projeto testa a Fase 2:
   - chamada em DM, tanto iniciando quanto recebendo;
   - supressão de ruído e cancelamento de eco;
   - sons;
   - painel de conexão.
2. Se algo falhar, ler o `snapcord.log` para diagnosticar.
3. Depois, seguir para a Fase 3 (chat).
