# MacroHelper (Python)

Alternative de gravação/replay de macros para o **Win-Auto-Updater** / `SingleUpdate.exe`.

Substitui o gravador C++ (hooks LL + `macro.txt`) por uma GUI Tkinter + replay com esperas
ativas, coordenadas relativas à janela e clique UIA quando disponível.

## Requisitos

- Windows 10/11 **x64**
- Python **3.11 ou 3.12** (para desenvolvimento)
- Preferir correr **como Administrador** (o SingleUpdate é elevado; o helper herda tokens
  quando é lançado por ele — standalone precisa de admin para clicar em janelas elevadas)

## Dependências

| Pacote | Papel |
|--------|--------|
| **PyAutoGUI** | Cliques e hotkeys no replay |
| **pywin32** | Janelas, processos, Activate / DPI |
| **pywinauto** + **comtypes** | Localizar/invocar controlos UIA |
| **pynput** | Captura global de rato/teclado (F10 consumido) |
| **Tkinter** | GUI de gravação (stdlib) |

```bat
cd py_src
py -3.12 -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

## CLI

```bat
REM Gravar (GUI)
python -m macro_helper --record --macro "D:\pen\macro.json"

REM Listar passos
python -m macro_helper --list "D:\pen\macro.json"

REM Replay (as 5 tools já devem estar abertas pelo SingleUpdate)
python -m macro_helper --play "D:\pen\macro.json"

REM Replay + lançar as tools (teste standalone)
python -m macro_helper --play "D:\pen\macro.json" --replay-launch

REM Continuar mesmo se um passo falhar
python -m macro_helper --play macro.json --on-fail continue --timeout 90
```

Exit code: `0` = sucesso, `≠0` = falha (o C++ usa isto).

## Formato `macro.json` (versão 1)

```json
{
  "version": 1,
  "created_at": "2026-10-16T15:00:00+00:00",
  "dpi_awareness": "per_monitor_v2",
  "steps": [
    {
      "type": "click",
      "dt_ms": 1200,
      "button": "left",
      "rel_x": 140,
      "rel_y": 62,
      "window": {
        "process": "powerMAX.exe",
        "class_name": "...",
        "title": "powerMAX",
        "monitor": 0,
        "dpi": 96
      },
      "uia": {
        "automation_id": "",
        "name": "Start",
        "control_type": "Button"
      }
    },
    {
      "type": "key",
      "dt_ms": 400,
      "vk": 27,
      "key": "esc",
      "modifiers": { "ctrl": true, "shift": false, "alt": false, "win": false },
      "window": { "process": "FurMark_GUI.exe", "class_name": "", "title": "", "monitor": 0, "dpi": 96 }
    }
  ]
}
```

- **Cliques**: botão L/R + `rel_x`/`rel_y` ao canto superior esquerdo da janela top-level +
  identidade (`process`, `class_name`, `title`) + UIA opcional + monitor/DPI.
- **Teclas**: `vk` + nome amigável + modificadores; a janela alvo é a foreground no momento.
- `dt_ms` é só diagnóstico — o replay **não** depende de tempos gravados; espera ativamente
  pela janela (timeout default 60 s).
- Não há compatibilidade com o antigo `macro.txt` — regrava a macro.

Ruído ignorado na gravação: `explorer.exe`, `cmd.exe`, `powershell.exe`, `conhost.exe`,
o próprio helper, `SingleUpdate.exe`, etc.

## Como gravar (passo a passo)

1. Na pen: `SingleUpdate.exe` + `MacroHelper.exe`. A pasta
   `Desktop\programas de teste\` tem de existir no PC Windows (não precisa de ir na pen
   se já estiver no Desktop).
2. Corre o SingleUpdate **como admin**.
3. **Record macro** → MacroHelper abre → **Iniciar gravação** (vermelho).
4. **Open test tools** — abre as 5 apps (MemTest por último, à frente).
5. Clica manualmente OK/Start/fullscreen / `Ctrl+Esc` nas apps enquanto grava.
6. **F10** — grava `Desktop\macro.json` (e abre o ficheiro).
7. Testa: **Run stress tests** (abre as tools + reproduz a macro).
8. Para a pen: copia `Desktop\macro.json` para a mesma pasta dos dois `.exe`.

A GUI fica *topmost*, não grava cliques sobre si própria, e o F10 é consumido.

### Botões

| Botão | O que faz |
|-------|-----------|
| **Open test tools** | Só abre as 5 apps (sem macro). MemTest fica por cima. |
| **Run stress tests** | Resolução máx. → abre as 5 apps → replay da macro. |
| **Record macro** | Abre o MacroHelper (gravação). |
| **Monitor** | OpenHardwareMonitor (+ BatteryCare em portáteis). |

## Replay (pelo SingleUpdate)

1. Resolução máxima (já feita pelo app).
2. Lança as 5 tools (FurMark, Prime95, CDM, powerMAX, MemTest).
3. Espera 4 s.
4. Corre `MacroHelper.exe --play "<pasta>\macro.json"`.
5. Por cada passo: espera a janela → ativa → UIA se existir → senão coordenadas relativas.
6. Log no stdout (capturado pelo SingleUpdate) e em `macro.json.log`.

## Build do `MacroHelper.exe` (Windows)

```bat
cd py_src
build_exe.bat
REM → dist\MacroHelper.exe
```

Ou manualmente:

```bat
pip install -r requirements.txt pyinstaller
pyinstaller --noconfirm MacroHelper.spec
```

O spec usa `--onefile` com **noconsole** (sem flash de consola na gravação). O modo `--play`
reatacha stdout aos handles herdados do SingleUpdate e escreve sempre `macro.json.log`.

## Deploy (pen)

```text
SingleUpdate.exe
MacroHelper.exe
macro.json          ← gravado uma vez
stress.ini          ← opcional (gerado pelo app)
```

`stress.ini` relevante:

```ini
UseMacro=1
MacroEngine=python
MacroFile=macro.json
```

`MacroEngine=cpp` volta ao replay C++ antigo (`macro.txt`) se precisares de fallback.

## Integração C++

| Ação SingleUpdate | Helper |
|-------------------|--------|
| Botão **Record macro** | `MacroHelper.exe --record --macro "<dir>\macro.json"` |
| **Run stress tests** / auto-run | Lança tools → `MacroHelper.exe --play "<dir>\macro.json"` |

## Validar em Windows

1. Build C++ (Linux ou MSYS2) → `SingleUpdate.exe` sem warnings.
2. Build Python → `MacroHelper.exe`.
3. Grava uma macro curta (1–2 cliques numa notepad/calc elevados se quiseres).
4. `--list` e `--play` standalone.
5. Fluxo real: stress tools + Record + F10 + Run stress tests noutro PC (mesma resolução máx.).
