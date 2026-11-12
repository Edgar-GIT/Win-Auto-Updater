# Testes nativos (beta)

Bancada de testes própria do Win Auto Updater: faz o trabalho do MemTest, Prime95 /
powerMAX, CrystalDiskMark e FurMark sem abrir nenhuma ferramenta externa e sem macro.

Tudo o que é desta funcionalidade vive nesta pasta. O resto do programa só ganhou um
botão (**Testes nativos**) que lança o mesmo executável noutro processo:

```
SingleUpdate.exe --native-tests                 # abre o painel
SingleUpdate.exe --native-tests --autostart=15  # abre e corre tudo durante 15 min
```

Corre num processo à parte de propósito: um reset do driver gráfico ou a RAM quase toda
ocupada pelo teste de memória nunca afetam o atualizador nem o fluxo da macro.

## O painel

- **Iniciar todos** — corre os quatro testes em simultâneo durante 5 / 15 / 30 / 60 min
  (ou até parar) e dá um veredicto: `APROVADO` ou `REPROVADO` com o motivo.
- Cada cartão também corre sozinho (Iniciar / Parar).
- **Relatório** — grava `Testes_<PC>_<data>.txt` no Ambiente de Trabalho.
- O PC não entra em suspensão nem apaga o ecrã enquanto houver testes a correr.
- Registo: `%TEMP%\WinAutoUpdater\native_tests.log`.

## Os testes

| Teste | Ficheiro | O que faz | O que conta como erro |
|---|---|---|---|
| RAM | `ram_test.*` | Reserva a memória livre (menos uma margem de segurança), fixa-a em RAM física e repete escrita → verificação com 8 padrões (aleatório, invertido, endereço, xadrez, bit andante, zeros, uns) em todos os núcleos. | Qualquer palavra lida diferente da escrita (guarda offset, esperado e lido). |
| CPU | `cpu_burner.*` | Um thread por núcleo lógico com multiplicação de matrizes (AVX2+FMA quando existe) e mistura de inteiros. | Resultado diferente da referência calculada no arranque. O "desempenho % do pico" mostra *throttling* térmico. |
| Disco | `disk_test.*` | As quatro linhas do CrystalDiskMark (SEQ1M Q8T1, SEQ1M Q1T1, RND4K Q32T1, RND4K Q1T1), leitura e escrita, com E/S sem cache num ficheiro temporário. | Blocos que voltam diferentes do que foi gravado (cada bloco de 4 KiB é auto-descritivo) e chamadas de E/S falhadas. |
| GPU | `gpu_test.*`, `gpu_shaders.hpp` | "Void Burner": cena *ray-marched* em Direct3D 11 (cemitério violeta, cubo void). *Burn* = carga contínua com resolução interna adaptativa; *Benchmark* = 60 s a 1280×720 fixos, pontuação comparável. | Reset do driver sob carga (dispositivo removido / pendurado). |

Notas:

- O teste de disco apaga sempre o ficheiro de teste (`FILE_FLAG_DELETE_ON_CLOSE`). Na
  sessão completa, depois da medição só faz leituras e verificações em ciclo, para não
  gastar o SSD.
- A cena da GPU é desenhada em faixas e sem v-sync: a gráfica fica a 100 % mas nenhum
  *draw* isolado demora o suficiente para disparar o *watchdog* do Windows (TDR) em
  gráficas fracas.
- Teclas na janela gráfica: `ESC` fecha, `F11` janela / ecrã inteiro, `H` esconde o painel.
- Sem driver gráfico instalado (renderização por software) o teste recusa-se a correr e
  avisa para instalar o driver.

## Shaders

O HLSL está em `gpu_shaders.hpp`. Por omissão é compilado no próprio PC com o
`d3dcompiler_47.dll` do Windows na primeira execução e guardado em cache em
`%TEMP%\WinAutoUpdater\shader_*.bin`. Se o shader principal falhar, entra uma cena de
recurso mais simples e o erro fica no registo.

Opcional: `tools/compile_shaders.sh <pasta com d3dcompiler_47.dll da Microsoft>` gera
`gpu_shader_bytecode.inc`; se esse ficheiro existir, o bytecode vai embutido no exe e
nada é compilado na máquina de teste.

## Desenvolvimento

Com `-DSU_UI_SNAPSHOT=ON` o executável aceita ainda:

```
--native-selftest <ficheiro.txt>   # RAM/CPU/disco durante segundos, com e sem falha injetada
--native-snapshot <pasta>          # PNGs do painel + composição cena/HUD
--gpu-snapshot <pasta>             # PNGs da cena em vários instantes
```

## Estado

Versão de teste. Verificado em Wine: os quatro testes, a deteção de falhas injetadas
(RAM e CPU), a verificação de dados do disco, a cena e a sessão completa. Falta validar
em Windows real: o compilador de shaders da Microsoft, drivers gráficos reais, a fixação
de memória (`VirtualLock`) e os valores do disco com E/S verdadeiramente sem cache.
