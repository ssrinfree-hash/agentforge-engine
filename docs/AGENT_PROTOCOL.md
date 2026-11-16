# AgentForge — протокол агента (AGENT_PROTOCOL)

Справочник JSON-протокола движка AgentForge (версия движка **0.1.0**). Протокол
спроектирован для LLM-агентов: одна строка stdin — один запрос, одна строка
stdout — один ответ. Всё машиночитаемо, никакого GUI.

## 1. Транспорт

Запуск:

```
afengine serve [--scene SCENE.afscene] [--catalog catalog.jsonl] [--weights asset_model.bin]
```

- Транспорт — **stdio, построчно**. Агент пишет в stdin одну строку с одним
  JSON-объектом `{"cmd":"...", ...}`; движок в ответ печатает одну строку с
  одним JSON-объектом и делает flush. Соединение keep-alive: сеанс живёт до
  команды `quit`.
- Пустые строки игнорируются. Некорректный JSON в строке → ответ
  `{"ok":false,"error":"bad request json: <текст>"}`, сеанс продолжается.
- Логи движка (прогресс загрузки каталога/весов, selftest) идут в **stderr** —
  их надо отличать от ответов в stdout.
- Ключи объектов в ответах отсортированы по алфавиту (движок сериализует через
  `std::map`); порядок ключей не значим, но ответы выглядят именно так.
- Ответ `{"cmd":"quit"}` содержит `"bye":true`, после его печати процесс
  движка завершается.

Общий вид ответа:

```json
{"ok": true}
```

```json
{"ok": false, "error": "текст ошибки"}
```

`ok:true` — команда выполнена; при `ok:false` поле `error` содержит
человекочитаемую причину (см. таблицу ошибок). Других кодов нет.

Полное описание нод и команд движок отдаёт сам: `{"cmd":"schema"}` — это
first-describing справочник, всегда доступный изнутри сеанса (то же печатает
`afengine schema` без сервера).

## 2. Команды

### 2.1 ping

Проверка живости и версии.

```json
{"cmd": "ping"}
```

```json
{"engine": "AgentForge", "ok": true, "time": 0.0, "version": "0.1.0"}
```

`time` — текущее время симуляции в секундах.

### 2.2 schema

Self-describing схема: типы нод со списками свойств, mesh-спецификация,
список команд, примеры.

```json
{"cmd": "schema"}
```

Ответ (сокращённо, массивы `props` перечисляют имена свойств каждой ноды):

```json
{
  "ok": true,
  "schema": {
    "commands": ["ping", "schema", "scene.new", "scene.load", "scene.save",
                 "scene.tree", "node.add", "node.set", "node.get", "node.remove",
                 "sim.step", "sim.reset", "raycast", "frame.capture",
                 "asset.search", "scores.set", "plugin.load", "plugin.call",
                 "selftest", "benchmark", "quit"],
    "engine": "AgentForge",
    "examples": {
      "frame.capture": {"cmd": "frame.capture", "height": 360, "save": "/tmp/frame.ppm", "width": 640},
      "node.add": {"cmd": "node.add", "node": {"mesh": {"primitive": "box", "size": [1, 1, 1]}, "name": "Crate", "props": {"color": [0.8, 0.4, 0.1], "position": [0, 0.5, 0], "static_body": true}, "type": "MeshInstance3D"}, "parent": "root"}
    },
    "mesh_spec": {
      "file": "путь к .obj/.off (OBJ с mtl Kd)",
      "height": "для capsule",
      "primitive": "box|plane|sphere|capsule",
      "radius": "для sphere/capsule",
      "size": "[x,y,z] для box; [s] для plane"
    },
    "node_types": [
      {"props": ["name", "type", "enabled"], "type": "Node"},
      {"props": ["name", "type", "enabled", "position", "rotation_euler_deg", "rotation_quat_xyzw", "scale"], "type": "Node3D"},
      {"props": ["name", "type", "enabled", "position", "rotation_euler_deg", "scale", "color", "visible", "static_body", "body", "mesh_class", "velocity"], "type": "MeshInstance3D"},
      {"props": ["name", "position", "rotation_euler_deg", "fov_deg", "near_z", "far_z", "active"], "type": "Camera3D"},
      {"props": ["name", "position", "rotation_euler_deg", "color", "intensity", "active"], "type": "Light3D"},
      {"props": ["target_path", "length", "time", "playing", "loop", "anim_class", "keyframes"], "type": "Animator"}
    ],
    "protocol": "json-rpc-lines: каждая строка stdin — один JSON-запрос {\"cmd\":...}; каждый JSON-объект stdout — ответ",
    "version": "0.1.0"
  }
}
```

### 2.3 scene.new

Сбрасывает текущую сцену: новое пустое дерево (корень `SceneTree` с именем
`root`), очищает кэш мешей, счётчики и лог событий плагина.

```json
{"cmd": "scene.new"}
```

```json
{"ok": true, "scene": {"name": "root", "type": "SceneTree"}}
```

`scene` здесь — компактное дерево (только `name`/`type`/`children`), а не полная
сцена.

### 2.4 scene.load

Загрузка сцены из файла или инлайном.

```json
{"cmd": "scene.load", "path": "scene.afscene"}
```

```json
{"ok": true, "scene": {"children": [{"name": "Ground", "type": "MeshInstance3D"}], "name": "root", "type": "SceneTree"}}
```

Вариант с инлайн-сценой (объект в поле `scene`, тот же формат, что и файл):

```json
{"cmd": "scene.load", "scene": {"format": "agentforge-scene", "version": 1, "time": 0, "scores": {}, "root": {"type": "SceneTree", "name": "root", "children": []}}}
```

- Формат файла описан в `docs/SCENE_FORMAT.md`. Путь к файлу и пути к мешам
  внутри сцены (`mesh.file`) разрешаются относительно **рабочего каталога
  процесса afengine** — запускай движок из каталога сцены.
- В версии 0.1.0 при загрузке из файла применяются `props` нод и сахарные поля
  `position`/`scale`/`rotation_euler_deg`/`color`/`visible`; поле `mesh` у нод
  из файла игнорируется — геометрию прикрепляй через `node.add`/`node.set`.

### 2.5 scene.save

Сохранение текущей сцены в файл (полный формат: props всех нод + дети).

```json
{"cmd": "scene.save", "path": "out.afscene"}
```

```json
{"ok": true, "path": "out.afscene"}
```

### 2.6 scene.tree

Компактное дерево текущей сцены (имена, типы, вложенность; без props).

```json
{"cmd": "scene.tree"}
```

```json
{"ok": true, "scene": {"children": [{"name": "Ground", "type": "MeshInstance3D"}, {"name": "Cam", "type": "Camera3D"}], "name": "root", "type": "SceneTree"}}
```

### 2.7 node.add

Добавление ноды. Ключевая команда построения сцены.

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "MeshInstance3D", "name": "Crate", "mesh": {"primitive": "box", "size": [1, 1, 1]}, "props": {"position": [0, 0.5, -3], "color": [0.8, 0.4, 0.1], "static_body": true, "mesh_class": "crate"}}}
```

```json
{"id": 2, "ok": true, "path": "root/Crate"}
```

Поля запроса:

- `parent` — путь родителя (см. «Правила путей» ниже); по умолчанию `root`.
- `node.type` — один из шести типов; неизвестный тип → ошибка (см. schema).
- `node.name` — уникально среди детей родителя; если пусто, движок назовёт ноду
  `<Type><id>`.
- `node.props` — объект свойств (любые свойства типа ноды).
- `node.mesh` — спецификация геометрии, разрешена только у `MeshInstance3D`
  (см. «Mesh-спецификация» ниже).
- Сахар: ключи `position`, `scale`, `rotation_euler_deg`, `color`, `visible`,
  `body`, `static_body`, `mesh_class`, `velocity` можно класть на верхний
  уровень объекта `node` — они применяются как props (сахар применяется ПОСЛЕ
  `props` и перекрывает его). Поворот задаётся в градусах: `rotation_euler_deg:
  [-18, 0, 0]`, порядок осей XYZ (применение: Z → X → Y, `R = Ry·Rx·Rz`).

Mesh-спецификация (поле `mesh`), эквивалент `mesh_spec` из schema:

```json
{"primitive": "box", "size": [1, 1, 1]}
```

```json
{"primitive": "plane", "size": [40]}
```

```json
{"primitive": "sphere", "radius": 0.4}
```

```json
{"primitive": "capsule", "radius": 0.3, "height": 1.8}
```

```json
{"file": "assets/chair_0001.obj"}
```

- `size` для box — полные габариты по трём осям; для plane — одно число (сторона
  квадрата в плоскости XZ, по умолчанию 10); `radius` по умолчанию 0.5,
  `height` по умолчанию 1.8.
- `file` — путь к `.obj` (цвет берётся из MTL `Kd`, если mtl есть) или `.off`
  (ModelNet40). Путь относителен рабочему каталогу afengine; меши кэшируются
  по пути.

### 2.8 node.set

Изменение свойств ноды (в т.ч. замена меша). Путь — без префикса `root/`.

```json
{"cmd": "node.set", "path": "Crate", "props": {"color": [0.9, 0.2, 0.2], "position": [1, 0.5, -2]}}
```

```json
{"node": {"color": [0.9, 0.2, 0.2], "enabled": true, "mesh_class": "crate", "mesh_name": "box", "mesh_tris": 12, "name": "Crate", "position": [1, 0.5, -2], "rotation_quat_xyzw": [0, 0, 0, 1], "scale": [1, 1, 1], "static_body": true, "type": "MeshInstance3D", "velocity": [0, 0, 0], "visible": true, "body": false}, "ok": true, "path": "Crate"}
```

`node` в ответе — полное self-описание ноды после изменений (props + вложенные
`children`, если есть). Замена геометрии: положи `mesh` внутрь `props`:

```json
{"cmd": "node.set", "path": "Crate", "props": {"mesh": {"file": "assets/barrel.obj"}}}
```

### 2.9 node.get

Полное self-описание ноды (props + children рекурсивно).

```json
{"cmd": "node.get", "path": "Crate"}
```

```json
{"node": {"body": false, "children": [], "color": [0.9, 0.2, 0.2], "enabled": true, "mesh_class": "crate", "mesh_name": "box", "mesh_tris": 12, "name": "Crate", "position": [1, 0.5, -2], "rotation_quat_xyzw": [0, 0, 0, 1], "scale": [1, 1, 1], "static_body": true, "type": "MeshInstance3D", "velocity": [0, 0, 0], "visible": true}, "ok": true}
```

Полезно после `node.set`, чтобы увидеть фактически применённые значения
(включая `rotation_quat_xyzw`, вычисленный из `rotation_euler_deg`).

### 2.10 node.remove

Удаление ноды из родителя (дети удаляются вместе с ней).

```json
{"cmd": "node.remove", "path": "Crate"}
```

```json
{"ok": true}
```

Путь указывается без префикса `root/` (для детей корня — просто имя).

### 2.11 sim.step

Шаг симуляции: обновление аниматоров (Animator) + физика (гравитация, AABB-коллизии
с землёй y=0 и статическими телами). Внутри каждого шага вызывается update
загруженного плагина.

```json
{"cmd": "sim.step", "frames": 120, "dt": 0.016}
```

```json
{"events": [{"event": "ground_hit", "impact": 7.65, "node": "root/Ball"}, {"event": "collision", "node": "root/Ball", "with": "root/Crate"}], "frame": 120, "frames": 120, "ok": true, "scores": {}, "time": 1.92}
```

- `frames` — сколько шагов (по умолчанию 1, ограничение 1..100000); `dt` — шаг
  в секундах (по умолчанию 1/60), движок ограничивает фактический dt сверху
  значением 0.05.
- Физика действует на ноды `MeshInstance3D` с `body:true` (гравитация −9.8 по
  Y, плоскость земли y=0, упругость 0.35, трение 0.86). Ноды с
  `static_body:true` — неподвижные коллайдеры.
- `events` — события шагов:
  - `{"event":"ground_hit","node":"root/Ball","impact":<скорость удара>}` —
    касание земли;
  - `{"event":"collision","node":"root/Ball","with":"root/Crate"}` — контакт с
    статическим телом.
  Пути в событиях — полные (с префиксом `root/`).
- `scores` — текущее игровое состояние (числовые счётчики, см. `scores.set`).

### 2.12 sim.reset

Сброс времени, номера кадра и scores (сцена не пересоздаётся).

```json
{"cmd": "sim.reset"}
```

```json
{"ok": true}
```

### 2.13 raycast

Луч из точки в направлении по AABB всех видимых меш-нод.

```json
{"cmd": "raycast", "origin": [0, 1.5, 5], "dir": [0, -0.2, -1], "max_dist": 50}
```

Попадание:

```json
{"ok": true, "raycast": {"dist": 5.1, "hit": true, "path": "root/Crate", "point": [0, 0.48, -0.1]}}
```

Промах:

```json
{"ok": true, "raycast": {"hit": false}}
```

`max_dist` по умолчанию 100. `point` — точка входа в AABB, `dist` — расстояние
до неё. Возвращается ближайшее пересечение.

### 2.14 frame.capture

Рендер кадра с активной камеры + FrameReport — «экран для агента».

```json
{"cmd": "frame.capture", "width": 320, "height": 180, "save": "frame.ppm"}
```

```json
{
  "frame": {
    "camera": {"fov_deg": 60, "position": [0, 2.5, 6]},
    "frame": 120,
    "objects": [
      {"class": "crate", "color": [0.9, 0.2, 0.2], "depth_range": [5.1, 6.3], "name": "Crate", "path": "root/Crate", "screen_bbox": [131, 84, 189, 129], "tris": 12, "type": "MeshInstance3D", "visible": true, "world_pos": [1, 0.5, -2]},
      {"class": "coin", "color": [1, 0.85, 0.1], "depth_range": [10.2, 10.3], "name": "Coin", "path": "root/Spin/Coin", "screen_bbox": [150, 96, 173, 114], "tris": 12, "type": "MeshInstance3D", "visible": true, "world_pos": [0, 1, -4]}
    ],
    "resolution": [320, 180],
    "scores": {},
    "sim_time": 1.92,
    "stats": {"drawn_tris": 24, "pixel_hash": 13929914952179248201, "render_ms": 0.42}
  },
  "ok": true,
  "saved_ppm": "frame.ppm"
}
```

- Активная камера — первая `Camera3D` с `active:true`; если её нет →
  `{"ok":false,"error":"no active Camera3D in scene"}`.
- `objects` перечисляет **все** видимые и включённые `MeshInstance3D`
  (проекция их мирового AABB; окклюзия не учитывается). Если объект целиком за
  камерой — `visible:false`, `screen_bbox:[0,0,0,0]`, `depth_range:[0,0]`.
  Координаты bbox — пиксели `[x0,y0,x1,y1]`, `depth_range` — `[близко, далеко]`
  в метрах от камеры; bbox может выходить за пределы кадра.
- `stats.drawn_tris` — реально растеризованные треугольники, `render_ms` —
  время растеризации, `pixel_hash` — FNV-1a хэш RGBA-буфера (uint64, в JSON как
  число) — быстрый способ сравнить два кадра «одинаково ли выглядит».
- `save` — путь для PPM-файла (P6); при задании в ответ добавляется
  `saved_ppm`. Если кадр не нужен, поля `save` можно не указывать.
- `width`/`height` по умолчанию — текущий вьюпорт (640×360).

### 2.15 asset.search

Семантический поиск ассета по тексту (модель hash-n-gram → 40 классов,
обученная на ModelNet40; каталог задаётся флагом `--catalog`, веса —
`--weights`).

```json
{"cmd": "asset.search", "query": "chair", "top_k": 3}
```

```json
{"ok": true, "search": {"model": "hash-ngram-softmax", "n_catalog": 12311, "query": "chair", "results": [{"class": "chair", "class_score": 0.87, "faces": 4812, "id": "chair_0001", "name": "office chair", "verts": 2714}, {"class": "chair", "class_score": 0.87, "faces": 3731, "id": "chair_0002", "name": "wooden chair", "verts": 2106}, {"class": "chair", "class_score": 0.87, "faces": 5980, "id": "chair_0003", "name": "arm chair", "verts": 3310}]}}
```

- Без загруженных весов работает fallback по подстроке (`"model":
  "substring-fallback"`, поле `class_score` в записях отсутствует).
- `top_k` по умолчанию 10 (ограничение 1..100).
- Найденный ассет добавляется в сцену обычным node.add:
  `{"file":"assets/chair_0001.obj"}` — каталог моделей лежит на диске рядом.

### 2.16 scores.set

Установка/обновление числовых счётчиков игрового состояния (score, hp, ...).

```json
{"cmd": "scores.set", "scores": {"score": 10, "hp": 3}}
```

```json
{"ok": true}
```

Счётчики возвращаются в `sim.step`, `frame.capture` и доступны плагину через
`score_get`/`score_set`.

### 2.17 plugin.load

Загрузка плагина игровой логики (.so, C ABI — полное описание в
`docs/PLUGIN_ABI.md`).

```json
{"cmd": "plugin.load", "path": "./game_logic.so"}
```

```json
{"ok": true}
```

- Открывает `.so` через dlopen, находит символы `af_plugin_init` и
  `af_plugin_update`, передаёт плагину структуру `AfApi` и сразу вызывает
  `af_plugin_init(api)`. Ненулевой код возврата → `ok:false`.
- Повторный `plugin.load` закрывает предыдущий плагин.

### 2.18 plugin.call

Одноразовый вызов опционального обработчика `af_plugin_event(const char* json)`.

```json
{"cmd": "plugin.call", "event": {"action": "restart"}}
```

```json
{"ok": true, "plugin": {"restarted": true}}
```

- Если плагин вернул валидный JSON, он кладётся в поле `plugin`; если нет —
  сырая строка в `plugin_raw`.
- Без загруженного плагина → `{"ok":false,"error":"no plugin loaded"}`.

### 2.19 selftest

Полный прогон внутренних проверок движка (математика, JSON, сцена, рендер,
физика, анимация, ассеты, протокол).

```json
{"cmd": "selftest"}
```

```json
{"checks": 20, "failed": 0, "ok": true, "results": [{"name": "math.quat_rotate_y90", "pass": true}, {"name": "physics.gravity_ground", "pass": true}, {"name": "protocol.capture_no_camera_error", "pass": true}]}
```

(показаны три примера из 20 проверок; полный список — в поле `results`).

### 2.20 benchmark

Микробенчмарки: растеризация, протокол, построение сцены, физика, поиск ассетов.

```json
{"cmd": "benchmark"}
```

```json
{"benchmark": {"physics_ms_per_600steps_20bodies": 12.4, "physics_steps_per_s": 48387, "protocol_calls_per_s": 210526, "protocol_scene_nodes": 100, "protocol_us_per_call": 4.75, "raster_ms_per_frame": 3.1, "raster_mtris_per_s": 19.35, "raster_resolution": "640x360", "raster_tris_per_frame": 60000, "scene_build_nodes_per_s": 142857, "scene_build_us_per_node": 7.0}, "ok": true}
```

(значения иллюстративные). Разрушает текущую сцену — вызывай до построения
арены или после сохранения.

### 2.21 quit

Завершение сеанса.

```json
{"cmd": "quit"}
```

```json
{"bye": true, "ok": true}
```

После печати ответа движок закрывает stdin/stdout и завершается.

## 3. Правила путей

Важная особенность версии 0.1.0 — двусторонняя запись путей:

- **В ответах** движка (node.add `path`, `scene.tree`, события физики, пути в
  FrameReport и raycast) путь всегда полный, с корнем: `root/Arena/Crate`.
- **В запросах** (node.get/set/remove, `parent` в node.add) путь указывается
  **без префикса `root/`** — от детей корня: `Arena/Crate`. Корень адресуется
  только спец. значением `"root"` в `parent` команды node.add.
- Имя ноды уникально среди детей одного родителя; `/` в имени ноды использовать
  нельзя (это разделитель путей).
- Animator ищет цель `target_path` **среди своих детей** (путь относительно
  Animator'а; пустой `target_path` — анимируется родитель Animator'а). Типовой
  шаблон: Animator висит в корне, анимируемый объект добавляется его ребёнком,
  `target_path` = имя объекта.

## 4. Таблица ошибок

Все ошибки возвращаются как `{"ok":false,"error":"..."}`. Тексты:

| error | когда возникает |
|---|---|
| `bad request json: <text>` | строка запроса не парсится как JSON |
| `no cmd field` | в запросе нет поля `cmd` |
| `unknown cmd: <cmd> (send {"cmd":"schema"})` | неизвестная команда |
| `exception: <text>` | внутренняя ошибка обработки (не падение процесса) |
| `need node object` | node.add без корректного `node` |
| `parent not found: <path>` | `parent` в node.add не существует |
| `unknown type: <type> (see schema)` | тип ноды не из шести поддерживаемых |
| `duplicate name under parent: <name>` | имя занято среди детей родителя |
| `mesh allowed only on MeshInstance3D` | поле `mesh` у не-Mesh ноды |
| `mesh: <причина>` | не удалось построить/загрузить меш (например `mesh: unknown primitive: cone`, `mesh: cannot open assets/x.obj`) |
| `need path + props` | node.set без `path` или `props` |
| `node not found: <path>` | нода по пути не найдена (проверь префикс `root/`) |
| `need path` | scene.save / plugin.load без `path` |
| `need path or scene` | scene.load без `path` и без `scene` |
| `cannot open <path>` | файл сцены не открылся |
| `bad scene json: <text>` | файл сцены — некорректный JSON |
| `scene load failed: <причина>` | сцена не собралась: `no root object`, `unknown node type: X`, `duplicate child name 'X' under 'Y'` |
| `need origin[3], dir[3]` | raycast без корректных векторов |
| `no active Camera3D in scene` | frame.capture при отсутствии активной камеры |
| `need query` | asset.search без `query` |
| `need scores object` | scores.set без объекта `scores` |
| `dlopen: <text>` | .so не загрузился (нет файла / не ELF / зависимости) |
| `missing af_plugin_init/af_plugin_update` | в .so нет обязательных символов |
| `af_plugin_init returned <N>` | плагин сам сообщил об ошибке инициализации |
| `no plugin loaded` | plugin.call до plugin.load |

## 5. Типовой сеанс агента

Сценарий: проверить движок → прочитать схему → построить арену → прокрутить
физику → снять кадр → найти ассет. Запуск: `cd dir_with_assets && afengine serve`.

```json
{"cmd": "ping"}
```

```json
{"engine": "AgentForge", "ok": true, "time": 0, "version": "0.1.0"}
```

```json
{"cmd": "schema"}
```

(смотри раздел 2.2 — по схеме агент уточняет имена свойств, не угадывая).

```json
{"cmd": "scene.new"}
```

```json
{"ok": true, "scene": {"name": "root", "type": "SceneTree"}}
```

Строим арену: пол, ящик (статика), мяч (динамика), монета на Animator,
камера и свет.

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "MeshInstance3D", "name": "Ground", "mesh": {"primitive": "plane", "size": [40]}, "props": {"position": [0, 0, 0], "color": [0.25, 0.5, 0.25], "static_body": true, "mesh_class": "floor"}}}
```

```json
{"id": 1, "ok": true, "path": "root/Ground"}
```

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "MeshInstance3D", "name": "Crate", "mesh": {"primitive": "box", "size": [1, 1, 1]}, "props": {"position": [0, 0.5, -3], "color": [0.8, 0.4, 0.1], "static_body": true, "mesh_class": "crate"}}}
```

```json
{"id": 2, "ok": true, "path": "root/Crate"}
```

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "MeshInstance3D", "name": "Ball", "mesh": {"primitive": "sphere", "radius": 0.4}, "props": {"position": [2, 3, -1], "body": true, "color": [0.9, 0.9, 0.2], "mesh_class": "ball"}}}
```

```json
{"id": 3, "ok": true, "path": "root/Ball"}
```

Монета вращается: сначала Animator-контейнер, потом объект внутри него
(`target_path` ищется среди детей Animator).

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "Animator", "name": "Spin", "props": {"target_path": "Coin", "playing": true, "loop": true, "anim_class": "spin", "keyframes": [{"t": 0, "pos": [0, 1, -4], "rot_quat_xyzw": [0, 0, 0, 1], "scale": [1, 1, 1]}, {"t": 1, "pos": [0, 1, -4], "rot_quat_xyzw": [0, 0.7071, 0, 0.7071], "scale": [1, 1, 1]}]}}}
```

```json
{"id": 4, "ok": true, "path": "root/Spin"}
```

```json
{"cmd": "node.add", "parent": "Spin", "node": {"type": "MeshInstance3D", "name": "Coin", "mesh": {"primitive": "box", "size": [0.5, 0.5, 0.05]}, "props": {"position": [0, 1, -4], "color": [1, 0.85, 0.1], "mesh_class": "coin"}}}
```

```json
{"id": 5, "ok": true, "path": "root/Spin/Coin"}
```

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "Camera3D", "name": "Cam", "props": {"position": [0, 2.5, 6], "rotation_euler_deg": [-18, 0, 0], "fov_deg": 60}}}
```

```json
{"id": 6, "ok": true, "path": "root/Cam"}
```

```json
{"cmd": "node.add", "parent": "root", "node": {"type": "Light3D", "name": "Sun", "props": {"rotation_euler_deg": [-55, -30, 0], "intensity": 1.2}}}
```

```json
{"id": 7, "ok": true, "path": "root/Sun"}
```

Два секунды симуляции: мяч падает и отскакивает от пола.

```json
{"cmd": "sim.step", "frames": 120}
```

```json
{"events": [{"event": "ground_hit", "impact": 7.65, "node": "root/Ball"}], "frame": 120, "frames": 120, "ok": true, "scores": {}, "time": 2.0}
```

Снимаем «экран» (значения в объектах иллюстративные):

```json
{"cmd": "frame.capture", "width": 320, "height": 180, "save": "frame.ppm"}
```

```json
{"frame": {"camera": {"fov_deg": 60, "position": [0, 2.5, 6]}, "frame": 120, "objects": [{"class": "floor", "color": [0.25, 0.5, 0.25], "depth_range": [5.5, 20.0], "name": "Ground", "path": "root/Ground", "screen_bbox": [-40, 96, 360, 180], "tris": 2, "type": "MeshInstance3D", "visible": true, "world_pos": [0, 0, 0]}, {"class": "crate", "color": [0.8, 0.4, 0.1], "depth_range": [8.3, 9.5], "name": "Crate", "path": "root/Crate", "screen_bbox": [131, 88, 189, 133], "tris": 12, "type": "MeshInstance3D", "visible": true, "world_pos": [0, 0.5, -3]}, {"class": "coin", "color": [1, 0.85, 0.1], "depth_range": [10.0, 10.1], "name": "Coin", "path": "root/Spin/Coin", "screen_bbox": [151, 97, 172, 113], "tris": 12, "type": "MeshInstance3D", "visible": true, "world_pos": [0, 1, -4]}], "resolution": [320, 180], "scores": {}, "sim_time": 2.0, "stats": {"drawn_tris": 26, "pixel_hash": 13929914952179248201, "render_ms": 0.4}}, "ok": true, "saved_ppm": "frame.ppm"}
```

Ищем ассет для расширения сцены:

```json
{"cmd": "asset.search", "query": "chair", "top_k": 3}
```

```json
{"ok": true, "search": {"model": "hash-ngram-softmax", "n_catalog": 12311, "query": "chair", "results": [{"class": "chair", "class_score": 0.87, "faces": 4812, "id": "chair_0001", "name": "office chair", "verts": 2714}, {"class": "chair", "class_score": 0.87, "faces": 3731, "id": "chair_0002", "name": "wooden chair", "verts": 2106}, {"class": "chair", "class_score": 0.87, "faces": 5980, "id": "chair_0003", "name": "arm chair", "verts": 3310}]}}
```

Сохраняем состояние и закрываем сеанс:

```json
{"cmd": "scene.save", "path": "arena.afscene"}
```

```json
{"ok": true, "path": "arena.afscene"}
```

```json
{"cmd": "quit"}
```

```json
{"bye": true, "ok": true}
```

## 6. Рекомендации для LLM-агентов

1. **Начинай с `schema`.** Первый запрос каждого сеанса — `{"cmd":"schema"}`:
   он даёт актуальные типы нод, имена свойств, mesh_spec и список команд. Не
   угадывай имена полей.
2. **Валидируй каждый ответ.** Проверяй `ok:true`; при `ok:false` читай
   `error` и исправляй запрос (чаще всего: не тот путь, дубликат имени, плохая
   mesh-спека). Ответ `node.add` содержит `path` — используй его для
   построения последующих путей (не забывая срезать префикс `root/` в
   запросах, см. «Правила путей»).
3. **Ставь `mesh_class` на каждый меш** ("crate", "coin", "wall", "chair" ...).
   Это семантическая метка: она попадает в FrameReport (`class`) и помогает
   рассуждать по кадру числами, а не пикселями.
4. **Дублируй важное состояние в `scores`** (score, hp, phase) через
   `scores.set`/плагин — FrameReport и sim.step возвращают их каждый раз, это
   дешёвая память состояния для агента.
5. **FrameReport — это твой экран.** Чтобы «посмотреть» сцену, читай
   `objects[].screen_bbox/depth_range/visible` и сравнивай кадры по
   `stats.pixel_hash`; PPM-файл нужен только для отладки человеком.
6. **Физика включается флагами**: статические препятствия — `static_body:true`,
   падающие/летящие объекты — `body:true`. Земля всегда y=0, гравитация по −Y.
7. **Анимация через Animator-контейнер**: объект-цель должен быть ребёнком
   Animator'а, `target_path` — имя цели (путь относительно Animator'а).
8. **`sim.step` пакетом**: одним вызовом с `frames:120` дешевле и быстрее, чем
   120 вызовов по кадру; события всех шагов придут одним списком.
9. **Не пиши в сцену мусорные имена** — они попадают в пути и отчёты; имя
   должно быть уникальным среди детей родителя и без символа `/`.
10. **Движок self-describing**: если документация и `schema` расходятся с
    реальным ответом — верь ответу движка и сообщай о расхождении в отчёте.
