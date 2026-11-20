# Tessera — plano completo

Documento de design. Resume todas as decisões e ideias discutidas até agora.
Sempre que algo for medido e se revelar diferente do que aqui está, atualizar este ficheiro.

---

## 1. Objetivo

Permitir que GPUs de 8 GB corram jogos pesados em VRAM:

- **Memória:** reduzir a VRAM usada até perto do *working set* real (ex.: FF7 Remake nos sewers, de ~11 GB
  para 7–8 GB realista; 6 GB como melhor caso), sem perda de qualidade percetível.
- **Zero stutters** de: falta de VRAM, compilação de shaders/PSOs e (na parte que é possível) traversal.
- **Zero pop-in** (garantido em D3D11; impercetível em D3D12).
- Tudo genérico: **sem trabalho manual por jogo**. No máximo, regras por motor (UE, Unity, …).

### Limite físico a ter sempre presente

O mínimo de VRAM "invisível" é o working set: texturas realmente vistas + render targets + geometria/BVH.
Abaixo disso, ou há perda de qualidade, ou pop-in. O Tessera deve **medir e mostrar** esse mínimo por jogo/zona.

A poupança vem de memória **residente mas não usada** (pools de streaming cheios de texturas fora do ecrã),
VRAM de outros processos, duplicados e texturas sem compressão.

---

## 2. Arquitetura

```
┌──────────────────────────┐   ficheiros   ┌───────────────────────────────┐
│ tessera.exe (preparação) │ ───────────▶ │ tessera.dll (dentro do jogo)   │
│ - deteta jogo/API/motor  │  caches       │ - proxy d3d11/d3d12/dxgi       │
│ - processa registos      │ ◀─────────── │ - patch de vtables             │
│ - chama ferramentas      │   registos    │ - memória, pacing, prefetch    │
└──────────┬───────────────┘               └───────────────────────────────┘
           │ processos separados
           ▼
  tessera_warm.exe (aquecimento de shaders, com o nome do exe do jogo)
  tessera_instrument.exe (DXC / DXBC)
  tessera_texcomp.exe (BC7/BC6H + FLIP)
```

### Ciclo de uso

1. Utilizador abre o Tessera → "Preparar". A app:
   - deteta API (imports do exe), motor, anti-cheat (recusa), hash do exe e versão do driver (invalida caches);
   - processa os registos da sessão anterior (shaders, texturas, co-ocorrência, I/O);
   - lê (só lê) configuração de outras ferramentas (RTSS, limite do painel NVIDIA);
   - aquece shaders fora do processo; instala a DLL (encadeando a que existir); fecha.
2. Utilizador lança o jogo normalmente (Steam, etc.).
3. A DLL: abre caches por memory-mapping → cria o device real e patcha vtables → deteta funcionalidades
   e componentes (Reflex, frame gen, streaming próprio, outras camadas) → pré-aloca o tile pool.
4. Durante o jogo: hooks só trabalham em criações de recursos; trabalho por frame no `Present`;
   thread de prefetch com prioridade de I/O baixa.
5. Ao fechar: grava registos. Nada fica a correr.

### Instalação da DLL e encadeamento

- D3D11 → proxy `d3d11.dll` (ou `dxgi.dll`). D3D12 → proxy `d3d12.dll` (uma `dxgi.dll` só vê o device a partir
  da swapchain e pode perder recursos criados antes).
- Se já existir uma DLL com o mesmo nome (ReShade, DXVK, Special K, OptiScaler, gravador do SCSKiller):
  renomear para `<nome>.next.dll` e carregá-la como camada seguinte (`next=` em `tessera.ini`).
  Desinstalar repõe o nome original.
- Patchar as vtables da **implementação real** (obtida ao criar o device na proxy), não de wrappers
  como o Streamline (`sl.interposer.dll`).
- Agility SDK: o patch de vtables funciona; confirmar que se patcham os objetos que o jogo realmente recebe.

---

## 3. Catálogo de otimizações

Formato: **realista** (primeira versão) → **potencial** (versão máxima).

### Memória (M)

- **M1 Orçamento ajustado** — hooks a `DXGI_ADAPTER_DESC::DedicatedVideoMemory` e `QueryVideoMemoryInfo`
  → também NVAPI/AGS; modo honesto automático para jogos com bom streaming (reporta o real menos o que o
  Tessera usa).
- **M2 Prioridades de residência** — D3D11 `SetEvictionPriority`, D3D12 `SetResidencyPriority`: render targets
  e buffers no máximo → prioridades dinâmicas por LRU.
- **M3 Tile pool + texturas tiled/reserved** — D3D11.2 tiled resources para texturas só de leitura
  (pedidas como `IMMUTABLE` → criadas `DEFAULT`, upload feito pelo Tessera); D3D12 reserved resources
  para texturas committed → heaps D3D12 só com texturas fornecidos de forma virtual. Heaps com aliasing
  ou render targets: nunca tocar.
- **M4 Residência LRU sob pressão** — enquanto couber no orçamento, não fazer nada; quando aperta,
  `SetResourceMinLOD`/`ResourceMinLODClamp` primeiro e só depois libertar tiles do mip maior das texturas
  mais frias → remoção gradual, um mip de cada vez, com histerese.
- **M5 Restauro sem pop-in** — mips removidos guardados em texturas STAGING (RAM legível pela GPU);
  D3D11: no hook do bind (`*SetShaderResources`), copiar o mip **antes do draw** (a GPU executa por ordem);
  limite de MB síncronos por frame (ex.: 32 MB); deferred contexts: restaurar antes do `ExecuteCommandList`.
  D3D12: fila de cópia própria + `ID3D12CommandQueue::Wait` antes do `ExecuteCommandLists` do jogo.
  Escritas do jogo em texturas geridas (`UpdateSubresource`, `CopySubresourceRegion`, `GenerateMips`)
  exigem mapear os tiles antes (Tier 2 descarta escritas em tiles não mapeados).
- **M6 Feedback de mips** — instrumentar shaders para registar (índice da textura, mip pedido):
  DXBC em D3D11 (formato simples, checksum conhecido) → DXIL em D3D12 (DXC, reassinar com `dxil.dll`),
  agregação por wave. Feito na preparação; shaders novos passam sem instrumentação.
- **M7 Previsão** — conjuntos de material (texturas associadas juntas) → grafo de co-ocorrência aprendido
  + margem de mip autoajustada pelas "atualizações atrasadas".
- **M8 Deduplicação** — texturas `IMMUTABLE` com o mesmo hash partilham o objeto (`AddRef`)
  → copy-on-write para `DEFAULT`.
- **M9 Recompressão com controlo de qualidade** — RGBA8 → BC7, RGBA16F → BC6H, só texturas de leitura,
  só se FLIP confirmar impercetível; feito offline na app, aplicado por hash na criação; hooks a `GetDesc`
  e `CreateShaderResourceView` para mentir sobre o formato (incl. sRGB) → D3D12 com troca de descritores.
  Nunca recomprimir saídas de NTC nem formatos não reconhecidos.
- **M10 Orçamento de RAM** — os mips removidos vão para a RAM: limite fixo → dinâmico + LZ4 dos mips em RAM.
- **M11 Medidor de working set** — mede o mínimo invisível por zona → sugere o orçamento ideal por jogo.

### Shaders (S) — objetivo: cobertura total sem o jogador ter de gravar

Facto medido pelo SCSKiller: na NVIDIA em D3D12, a cache do driver é por **stage** e depende só dos shaders e
dos bytes da root signature (estado sintetizado acerta; exceção: view instancing). Logo, na NVIDIA o problema
é "todos os shaders + root signature certa", não uma explosão de combinações.

- **S1 Procura exaustiva de bytecode** — todos os ficheiros da instalação, incluindo o exe e as DLLs;
  tentar zlib, zstd, LZ4, Oodle, Brotli em blocos comprimidos; validar containers DXBC/DXIL. Arquivos
  encriptados precisam de chave (pedir ao utilizador quando aplicável).
- **S2 Caches de PSOs do motor** — Unreal: `.stable.upipelinecache` (empacotado nos containers IoStore),
  com descrições completas de PSOs gravadas pelos developers. Formato próprio do Tessera alinhado com o
  **SODB** (Advanced Shader Delivery, SQLite, especificação pública da Microsoft).
- **S3 Root signatures** — embutidas (RTS0) → exatas; blobs serializados nos ficheiros/exe; regras por motor
  (o SCSKiller tem regras para UE 4.20–5.7); **captura automática no arranque**: a app lança o jogo até ao
  menu, a DLL regista as root signatures e PSOs globais, a app fecha-o. Motores bindless têm poucas root
  signatures → cruzá-las com todos os shaders.
- **S4 Pipelines válidos** — NVIDIA: cada stage num pipeline válido qualquer (emparelhar por assinaturas,
  "cobertura por stage"). AMD: VS compilado conforme o PS e o estado → gerar variantes prováveis de formatos.
- **S5 Aquecimento fora do processo** — `tessera_warm.exe` copiado com o nome do exe do jogo (a cache é
  indexada pelo nome do exe). Fallback dentro do processo no arranque quando a chave não bate certo
  (perfis AMD que trocam a chave, devices criados via AGS). D3D11 (NVIDIA): draw para um alvo 1×1 com
  argumentos indiretos de contagem zero. Nunca matar o processo de aquecimento a meio; nunca correr ao
  mesmo tempo que o jogo; relançar isolando itens suspeitos se o device for removido.
- **S6 Autocorreção** — a DLL regista passivamente as compilações residuais durante o jogo normal;
  a preparação seguinte compila-as. **Medidor de cobertura** na app (encontrados / compilados / falhados).
- **S7 Cache do driver** — verificar que está em ilimitado (NVIDIA); respeitar o limite de 16 GiB no AMD;
  recompilar após atualização do driver.
- Impossível sem correr o jogo: shaders compilados em runtime a partir de HLSL, ray tracing no AMD (só
  repetições exatas), arquivos encriptados sem chave, view instancing na NVIDIA.

### Streaming e I/O (I)

- **I1 Registo de leituras** — hooks a `CreateFileW`/`ReadFile` só na sessão de aprendizagem.
- **I2 Prefetch pela lista** — thread na DLL com prioridade de I/O baixa → limitado à RAM disponível.
- **I3 Prefetch previsivo** — ligar leituras ao grafo de texturas.
- **I4 Alocações instantâneas** — criar textura = recurso virtual + tiles do pool pré-alocado.
- **I5 Upload repartido** — mips pequenos primeiro; mip completo em partes.

### Pacing e sistema (P) — tudo só ao nível do processo

- **P1 Limitador preciso** — timer de alta resolução + espera ativa final. Regra: o limitador mais baixo
  manda; o Tessera fica ligeiramente abaixo do limite mais baixo encontrado (RTSS, painel NVIDIA), a não
  ser que o utilizador tenha posto de propósito um limite mais baixo noutra ferramenta.
- **P2 Flip model + tearing** para D3D11 em janela sem bordas.
- **P3 Latência de frames** — `SetMaximumFrameLatency`, waitable object.
- **P4 CPU sets** — thread que chama `Present` em P-cores (CPUs híbridos).
- **P5 Processo** — resolução do timer, EcoQoS desligado (`SetProcessInformation`), prioridade moderada.
- **P6 Recuo** — com Reflex ou frame generation, o limitador desliga-se (resto continua).

### Diagnóstico (D)

- **D1** Frametimes (QPC por `Present` mais exterior) + eventos da API, sempre ligado e barato.
- **D2** Relatório de stutters por causa, feito na app: shaders, criação de recursos, VRAM acima do orçamento,
  I/O, pacing, lógica do jogo.
- **D3** Teste de sistema opcional, na app, fora do jogo: DPC, disco, throttling.
- **D4** Estatísticas no OSD do RTSS (opcional; sem overlay próprio).

### Infraestrutura e convivência (C)

- **C1** DLL proxy, deteção de API, instalação/desinstalação limpas.
- **C2** Encadeamento de DLLs existentes.
- **C3** Exclusão por módulo (endereço de retorno + `GetModuleHandleEx` na criação de recursos).
- **C4** Deteção de funcionalidades (`CheckFeatureSupport`) e degradação por módulo.
- **C5** Modo seguro (contador de crashes).
- **C6** Invalidação de caches por hash do exe e versão do driver.
- **C7** Deteção de anti-cheat → recusa.
- **C8** Deteção de streaming próprio do jogo / NTC → recuar nesses recursos e no orçamento.

---

## 4. Problemas fora de alcance (só avisar)

Antivírus a analisar ficheiros, overlays de terceiros, tarefas de fundo, RAM do sistema esgotada e pagefile,
DPC de drivers, throttling, overclocks/XMP instáveis, SSD lento, polling de rato muito alto,
garbage collection, picos de lógica do jogo, DRM, a parte de CPU do traversal.

---

## 5. Roadmap

| Fase | Entra | Critério para avançar |
|---|---|---|
| 0. Fundações | C1, C2, C5, C6, C7, D1 (D3D11) | Jogo corre igual, custo < 1% |
| 1. Ganhos seguros | M1, M2, P1–P3, P5, P6, S7, D2 básico | Pacing melhor, sem regressões |
| 2. Shaders | S1, S2, S5 (D3D11 NVIDIA + D3D12), S6 + medidor | Zero stutters de compilação na 2.ª sessão |
| 3. Memória D3D11 | M3, M4, M5, M10, M11, C3, C4 | VRAM a descer, zero restauros atrasados |
| 4. Previsão e I/O | M7, M8, I1–I5, P4 | Menos stutters de traversal, pop-in ~0 |
| 5. Qualidade fina | M6 (DXBC), M9 | Poupança extra sem diferença na FLIP |
| 6. D3D12 | M1/M2/P/S → M3/M4 committed → C8 → M6 (DXIL) → M7; S3 captura no arranque | FF7 Remake: VRAM a descer, zero stutters de PSO |
| 7. Expansão | D3, D4, M9 em D3D12, heaps virtuais, AMD (S4) | Funciona com mods/overlays populares |

Jogos de teste: um jogo D3D11 pesado em VRAM (fases 0–5); FF7 Remake (D3D12) na fase 6.

### Fase 0 — tarefas concretas

1. Repositório: CMake raiz, `src/dll`, `src/app`, `src/common` (formatos, hashing), `tools/`, `tests/`.
2. `tessera.dll` como proxy `d3d11.dll`: encaminhar todas as exports para a DLL do sistema
   (`GetSystemDirectoryW`) ou para a `next=` do `tessera.ini`.
3. Utilitário de patch de vtables (guardar original, `VirtualProtect`, proteção de reentrância).
4. Hooks de registo: `CreateTexture2D`, `CreateBuffer`, `CreateShaderResourceView`, `Present`.
5. Registo binário por sessão em `%LOCALAPPDATA%\Tessera\<jogo>\` (recursos, frametimes).
6. Modo seguro: contador de arranques/crashes.
7. App mínima: lista de jogos por pasta escolhida, deteção de API pelos imports, deteção de anti-cheat
   (ficheiros conhecidos: EasyAntiCheat, BattlEye, etc.), instalar/desinstalar com encadeamento.
8. Medir: overhead da DLL vs sem DLL; relatório de onde vem a VRAM.

---

## 6. Hipóteses por validar (medir, não assumir)

- Custo real de `UpdateTileMappings` e da amostragem de tiled resources em cada driver.
- Custo da instrumentação de shaders (alvo: poucos %).
- D3D11 no AMD: gravando combinações completas de estado, consegue-se aquecer? (o SCSKiller só aquece
  D3D11 na NVIDIA).
- Aceitação de DXIL modificado (reassinatura com `dxil.dll`).
- Percentagem de texturas tiled-áveis em jogos D3D11 reais.
- Quanto do pool de streaming do UE4 está fora do ecrã nos sewers do FF7 Remake.

---

## 7. Referências

- **SCSKiller** (GPL-3.0-or-later, reutilizável): https://github.com/BlueHeisenberg/SCSKiller —
  ler `ARCHITECTURE.md`. Copiar: planos só com hashes, aquecimento com cópia do exe, comportamento medido
  (`selftest`), gravador com caixa de entrada e limite de tamanho, recuperação de device removido,
  deteção de atualização do driver, só ler ficheiros em jogos com anti-cheat.
- **CUE4Parse** (leitura de paks/IoStore do Unreal, C# — portar lógica), **ree-pak-rs** (RE Engine).
- **Advanced Shader Delivery / SODB**: https://microsoft.github.io/DirectX-Specs/d3d/StateObjectDatabase.html
- **Unreal bundled PSO caches**: documentação da Epic sobre `.upipelinecache` / `.spc` / `.shk`.
- **NVIDIA RTX Neural Texture Compression** (RTX Kit): o Tessera convive, não substitui.
- Bibliotecas: Dear ImGui, xxHash, bc7enc_rdo, NVIDIA FLIP, DXC, Catch2.
- Referências de técnica (só estudo): DXVK, Special K, ReShade.
