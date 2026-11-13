# Testes nativos (beta)

Os testes próprios do Win Auto Updater: fazem o trabalho do MemTest, Prime95 / powerMAX,
CrystalDiskMark, FurMark, BatteryCare e do monitor de sensores sem abrir nenhuma
ferramenta externa e sem macro.

Tudo o que é desta funcionalidade vive nesta pasta. O resto do programa só ganhou dois
botões e um interruptor, que lançam o mesmo executável noutro processo:

```
SingleUpdate.exe --native-tests --auto     # tudo sozinho, com veredicto no fim
SingleUpdate.exe --native-tests --manual   # uma janela por teste, para clicar à mão
```

Corre num processo à parte de propósito: um reset do driver gráfico ou a RAM quase toda
ocupada pelo teste de memória nunca afetam o atualizador nem o fluxo da macro.

## Os dois modos

- **Nativos auto** — abre o painel e começa logo: gráfica e disco primeiro, CPU e RAM
  assim que a cena gráfica está no ecrã. No fim dá `APROVADO`, `APROVADO COM AVISOS`
  (bateria gasta, temperaturas muito altas) ou `REPROVADO` com o motivo. `ESC` no ecrã
  do teste gráfico interrompe a sessão; "Repetir" corre outra vez.
- **Nativos manual** — cinco janelas pequenas (memória, processador, disco, gráfica,
  bateria), cada uma com o seu "Iniciar / Parar", como ter as ferramentas abertas lado a
  lado.
- **Depois de atualizar** — o interruptor "Usar os testes nativos após atualizar" do
  ecrã inicial escolhe entre estes testes (modo auto) e a bancada predefinida
  (ferramentas + macro). Fica gravado em `helpers\stress.ini` (`NativeTests=0/1`), para
  sobreviver aos reinícios da atualização.

## Duração: 5 minutos no relógio do teste gráfico

Não há duração para escolher. Cada teste dura 5 minutos contados no relógio do teste
gráfico (`TestClock`, em `nt_core`): enquanto a cena está a ser desenhada, o relógio só
avança 1/10 s por frame. Num PC rápido (10 FPS ou mais) são 5 minutos reais; a 5 FPS são
10; a partir de 3,3 FPS para baixo fica no teto de 15 minutos reais. Sem o teste gráfico
a correr (modo manual, ou PC sem driver gráfico) o relógio é o tempo real.

## A barra de monitorização

Faixa sempre por cima no lado esquerdo do ecrã (`monitor_bar.cpp`), também por cima do
teste gráfico em ecrã inteiro: tempo de teste, FPS (atual / médio / mín / máx),
temperatura e carga da gráfica, ventoinha, memória de vídeo, carga, temperatura e
frequência do processador, memória, disco, bateria e o total de erros. Tem "Parar",
"Relatório" e um `×` que fecha tudo.

De onde vêm as leituras (`sensors.cpp`), sem instalar drivers:

| Leitura | Fonte | Quando não há |
|---|---|---|
| Temperatura da gráfica, ventoinha | Núcleo gráfico do Windows (WDDM 2.4+, o que o Gestor de Tarefas usa); senão a biblioteca do driver NVIDIA (`nvapi64.dll`) ou AMD (`atiadlxx.dll`) | Gráficas integradas e drivers antigos: `n/d` |
| Carga da gráfica, memória de vídeo | Contadores `GPU Engine` / `GPU Adapter Memory` (Windows 10 1709+) | `n/d` |
| Temperatura do processador | Zona térmica ACPI mais quente (contador `Thermal Zone Information`) | Em algumas placas de secretária não existe: `n/d` |
| Frequência do processador | Contador `% Processor Performance` × frequência nominal | `n/d` |
| Temperatura do disco | Consulta de temperatura do armazenamento (Windows 10+) | `n/d` |

A temperatura por núcleo do processador exige um driver de kernel (é o que o HWMonitor
instala); por isso o botão "Sensores" do programa continua a existir.

## Os testes

| Teste | Ficheiro | O que faz | O que conta como erro |
|---|---|---|---|
| RAM | `ram_test.*` | Reserva a memória livre (menos uma margem de segurança), fixa-a em RAM física e repete escrita → verificação com 8 padrões em todos os núcleos. | Qualquer palavra lida diferente da escrita (guarda offset, esperado e lido). |
| CPU | `cpu_burner.*` | Um thread por núcleo lógico com multiplicação de matrizes (AVX2+FMA quando existe) e mistura de inteiros. | Resultado diferente da referência calculada no arranque. O "% do pico" mostra *throttling* térmico. |
| Disco | `disk_test.*` | As quatro linhas do CrystalDiskMark, leitura e escrita, com E/S sem cache num ficheiro temporário; depois lê e verifica em ciclo até ao fim do tempo (só leituras, não gasta o SSD). | Blocos que voltam diferentes do que foi gravado e chamadas de E/S falhadas. |
| Gráfica | `gpu_test.*`, `gpu_shaders.hpp` | "Void Burner": cena *ray-marched* em Direct3D 11 (cemitério violeta, cubo void), sempre em ecrã inteiro na resolução máxima do ecrã, desenhada 1:1 e sem v-sync. Em portáteis híbridos usa a gráfica dedicada. | Reset do driver sob carga (dispositivo removido / pendurado). |
| Bateria | `battery.*` | Lê o que a própria bateria indica: capacidade de fábrica e atual, carga, potência de carga / descarga, tensão, ciclos. Saúde = capacidade atual ÷ de fábrica: ≥ 80 % boa, 50–80 % usada, < 50 % gasta. | Não dá erro: uma bateria gasta aparece como aviso no veredicto. |

Notas:

- **Resolução máxima**: antes de abrir a cena, o teste gráfico põe o ecrã na resolução
  mais alta que suporta (a mesma chamada que a bancada da macro faz antes do FurMark) e
  a alteração fica gravada. `--keep-resolution` salta este passo.
- A cena é desenhada em faixas, mais finas quanto mais lenta a gráfica (≈ 25 ms cada):
  a gráfica fica a 100 % mas nenhum pedaço de trabalho demora o suficiente para disparar
  o *watchdog* do Windows (TDR). A "pontuação" é o FPS médio normalizado a 1920×1080.
- O teste de disco apaga sempre o ficheiro de teste (`FILE_FLAG_DELETE_ON_CLOSE`).
- Sem driver gráfico instalado (renderização por software) o teste gráfico recusa-se a
  correr e avisa para instalar o driver; os outros continuam.
- O PC não entra em suspensão nem apaga o ecrã enquanto houver testes a correr.
- **Relatório** grava `Testes_<PC>_<data>.txt` no Ambiente de Trabalho (veredicto,
  tempos, temperaturas máximas, resultados de cada teste, bateria).
- Registo: `%TEMP%\WinAutoUpdater\native_tests.log`.

## Estrutura

| Ficheiro | Papel |
|---|---|
| `native_tests.*` | Arranque: barra + painel automático ou janelas manuais |
| `bench.*` | Dono dos testes e sensores; duração, sessão, veredicto, relatório |
| `nt_core.*` | Utilitários e o relógio de teste |
| `panel.*`, `cards.*` | Base das janelas e o cartão de cada teste (partilhado pelos dois modos) |
| `dash_window.cpp`, `tool_window.cpp`, `monitor_bar.cpp` | As três janelas |
| `sensors.*`, `battery.*` | Leituras do sistema |

## Shaders

O HLSL está em `gpu_shaders.hpp`. Por omissão é compilado no próprio PC com o
`d3dcompiler_47.dll` do Windows na primeira execução e guardado em cache em
`%TEMP%\WinAutoUpdater\shader_*.bin`. Se o shader principal falhar, entra uma cena de
recurso mais simples e o erro fica no registo.

Opcional: `tools/compile_shaders.sh <pasta com d3dcompiler_47.dll da Microsoft>` gera
`gpu_shader_bytecode.inc`; se esse ficheiro existir, o bytecode vai embutido no exe e
nada é compilado na máquina de teste.

## Desenvolvimento

Opções extra do modo normal: `--test-seconds=N` (duração de cada teste) e
`--keep-resolution`. Com `-DSU_UI_SNAPSHOT=ON` o executável aceita ainda:

```
--native-selftest <ficheiro.txt>   # RAM/CPU/disco com e sem falha injetada, sensores, relógio
--native-snapshot <pasta>          # PNGs do painel, da barra e das janelas manuais
--gpu-snapshot <pasta>             # PNGs da cena em vários instantes
```

## Estado

Versão de teste. Verificado em Wine: os testes de RAM, CPU e disco (incluindo a deteção
de falhas injetadas), a cena, a sessão automática completa, a paragem automática de cada
teste no modo manual e o relógio de teste. Falta validar em Windows real: as leituras de
temperatura (o Wine não tem nenhuma das fontes), a leitura detalhada da bateria, a troca
de resolução, a escolha da gráfica dedicada, o compilador de shaders da Microsoft, a
fixação de memória (`VirtualLock`) e os valores do disco com E/S verdadeiramente sem
cache.
