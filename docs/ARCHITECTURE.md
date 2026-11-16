# AgentForge — игровой движок для ИИ-агентов

**AgentForge (AF)** — 3D-движок, спроектированный так, что его «пользователь» — не человек
у монитора, а LLM-агент. Всё управление — машиночитаемое: JSON-сцены, JSON-RPC-протокол
по stdio, FrameReport вместо экрана, self-describing схема нод.

## Принципы (отличия от классических движков)

| Классический движок (Godot/Unity) | AgentForge |
|---|---|
| GUI-редактор, человек кликает | Агент строит сцену JSON-командами по протоколу |
| Экран = окно для человека | Экран = FrameReport (JSON: объекты, bbox на «экране», глубина) + offscreen кадры |
| GDScript/C# внутри редактора | Ядро и логика — C++17 (плагин ABI), C# — через AgentBridge SDK (stdio) |
| Документация для человека | `afengine schema` печатает всю иерархию нод/свойств в JSON; docs/ для LLM |
| Ассеты ищет человек | Asset Intelligence: обученная модель text→asset (ModelNet40, 12 311 моделей) |

## Архитектура

```
afengine (бинарник, C++17, ноль внешних зависимостей)
├── core        math (Vec/Mat/Quat/AABB), json (парсер/сериализатор), log
├── scene       SceneTree: Node → Node3D → {MeshInstance3D, Camera3D, Light3D, Animator}
│               Node2D-уровень — над той же иерархией (2D = 3D с камерой-ортографикой)
├── render      software-растеризатор (z-buffer, Lambert+ambient, текстуры PPM),
│               RGBA8 framebuffer + float32 depth, offscreen PPM/RAW вывод
├── assets      OBJ+MTL, OFF; каталог ассетов; поиск (обученные веса, 40 классов)
├── physics     гравитация, AABB-коллизии, плоскость земли, raycast
├── anim        keyframe-треки (pos/quat/scale), линейная интерполяция, loop
├── agent       JSON-RPC stdio: scene.*, node.*, sim.*, frame.*, asset.*, plugin.*, selftest
└── game        плагин ABI (C ABI dlopen): update(dt), on_event(json) — игровая логика
```

## Форматы

- **Сцена**: `.afscene` — JSON: `{root:{type,name,transform,children:[...]}}` (Godot-подобно, но текст).
- **FrameReport**: на каждый кадр — `{frame, camera, resolution, objects:[{name,type,screen_bbox,depth,visible}], stats}` — это и есть «экран для нейросети»: агент видит сцену числами, человек ничего не открывает.
- **Протокол**: строка-запрос JSON → строка-ответ JSON (stdout), `afengine serve`.

## Обучение (Kaggle)

Ядро Kaggle: (1) сканирует 12 311 OFF-моделей ModelNet40 → каталог `catalog.jsonl`
(класс, вершины, грани, bbox, размер); (2) обучает модель `hash-ngram(text) → класс ассета`
(40-way softmax, PyTorch; TPU→GPU→CPU autodetect); (3) экспортирует веса `asset_model.bin`
+ каталог; движок грузит их и отвечает на `asset.search {"query":"chair"}` топ-K ассетов.

## Тестовая игра

`OrbRun` — 3D-арена: игрок-капсула, вращающиеся монеты (анимация), гравитация,
счёт, победа. Строится агентом по протоколу, логика — C++ плагин (+ C# версия в SDK),
playthrough headless → game report JSON + кадры.

## C# SDK

`scripts/csharp/AgentForge.cs` — AgentBridge (Process + stdin/stdout JSON),
типизированные вызовы сцены/симуляции; пример игры. Компиляция: `dotnet`/`mono`
(в песочнице нет — SDK поставляется исходником, ABI-путь C++ протестирован).
