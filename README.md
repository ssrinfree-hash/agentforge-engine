# AgentForge — игровой движок для ИИ-агентов

**AgentForge (AF)** — 3D-игровой движок, «пользователь» которого не человек у монитора,
а LLM-агент. Весь цикл создания игры — построение сцены, симуляция, визуальная проверка,
игровая логика — делается через машиночитаемые интерфейсы:

- **JSON-RPC по stdio** (`afengine serve`) — построение и изменение сцены, симуляция, raycast;
- **FrameReport** вместо экрана — агент «видит» кадр как JSON: объекты, экранные bbox,
  глубина, счёт игры; человек ничего не открывает;
- **self-describing**: `afengine schema` печатает все типы нод, свойства и команды;
- **самотестирование**: `afengine selftest` — 20 встроенных проверок ядра;
- **плагины C ABI** (`af_plugin_init/update/event`) — игровая логика на C++ без пересборки движка;
- **C# SDK** (`scripts/csharp/AgentForge.cs`) — типизированный AgentBridge поверх протокола;
- **Blender-мост** (`scripts/blender/blender_bridge.py`) — экспорт сцен Blender → `.afscene`;
- **Asset Intelligence** — модель `text → класс ассета`, обученная на Kaggle (ModelNet40,
  12 311 3D-моделей): `{"cmd":"asset.search","query":"chair"}` → топ-K ассетов из каталога.

Языки: ядро и игровая логика — **C++17** (ноль внешних зависимостей: только stdlib + dlopen),
игровая логика для .NET-агентов — **C#**. Приоритет — 3D (2D/2.5D реализуются через ту же
иерархию нод с ортографической камерой). Рендер — offscreen software-растеризатор
(z-буфер, Lambert + ambient), поэтому движок работает headless где угодно.

## Быстрый старт

```bash
# сборка (Linux/macOS, g++ / clang++)
git clone https://github.com/fkfkfmfkgkg/agentforge-engine.git
cd agentforge-engine && make -j
./build/afengine selftest        # 20/20 проверок ядра

# агентский сеанс: строим сцену и «смотрим» кадр
./build/afengine serve <<'EOF'
{"cmd":"scene.new"}
{"cmd":"node.add","parent":"root","node":{"type":"Camera3D","name":"Cam","position":[0,1,3]}}
{"cmd":"node.add","parent":"root","node":{"type":"Light3D","name":"Sun","props":{"intensity":2}}}
{"cmd":"node.add","parent":"root","node":{"type":"MeshInstance3D","name":"Crate",
  "mesh":{"primitive":"box","size":[1,1,1]},"position":[0,0.5,-2],
  "props":{"color":[0.8,0.4,0.1],"mesh_class":"crate"}}}
{"cmd":"frame.capture","width":640,"height":360,"save":"/tmp/frame.ppm"}
{"cmd":"quit"}
EOF
```

`frame.capture` вернёт FrameReport: экранные bbox каждого объекта (`screen_bbox`),
`depth_range`, `pixel_hash`, счёт игры — это и есть «экран для нейросети».

## Тестовая игра OrbRun (собирается агентом за 6 фаз)

```bash
make plugin                                   # C++ плагин игровой логики
python3 scripts/orbrun_build.py               # 6 фаз через протокол + playthrough
# → game/game_report.json: {"score":12,"win":true,...}, кадры в game/frames/
```

## Документация (написана для LLM)

| Файл | Что внутри |
|---|---|
| [docs/AGENT_PROTOCOL.md](docs/AGENT_PROTOCOL.md) | полный справочник JSON-RPC: 21 команда, примеры |
| [docs/SCENE_FORMAT.md](docs/SCENE_FORMAT.md) | формат `.afscene`, все 6 типов нод |
| [docs/PLUGIN_ABI.md](docs/PLUGIN_ABI.md) | C ABI плагинов игровой логики |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | архитектура и принципы |
| [docs/llms.txt](docs/llms.txt) | компактный индекс для LLM |
| [BENCHMARKS.md](BENCHMARKS.md) | измерения движка + качество asset-модели |

## Установка из релиза

```bash
# бинарник без сборки (linux x64):
wget https://github.com/fkfkfmfkgkg/agentforge-engine/releases/download/v0.1.0/afengine-linux64.tar.gz
tar xzf afengine-linux64.tar.gz && ./afengine selftest
```

## Состав

```
include/af/  math.h json.h scene.h render.h assets.h sim.h agent.h
src/         json scene render assets sim agent selftest benchmark main (.cpp)
game/        game_logic.cpp (OrbRun, C ABI) + orbrun_spec.json (Agnes) + отчёты
scripts/     kaggle/asset_kernel.py · csharp/ (SDK) · blender/ · orbrun_build.py
docs/        AGENT_PROTOCOL · SCENE_FORMAT · PLUGIN_ABI · ARCHITECTURE · llms.txt
tests/       selftest встроен в движок (afengine selftest)
```

## Обучение Asset Intelligence (Kaggle)

Ядро `scripts/kaggle/asset_kernel.py` (TPU→GPU→CPU autodetect): сканирует 12 311 OFF-моделей
ModelNet40 → `catalog.jsonl`; обучает hash-ngram → класс (linear softmax); экспортирует
`asset_model.bin` (формат AFASSET1) + `catalog.jsonl`. Движок грузит их флагами
`--catalog/--weights` и отвечает на `asset.search`. Метрики: holdout accuracy, MRR@5 на
синоним-запросах (см. BENCHMARKS.md).

## Лицензия

MIT. Идеи архитектуры сцен вдохновлены Godot Engine (MIT) и Blender-пайплайнами —
AgentForge написан с нуля, без кода этих проектов.
