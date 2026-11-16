# Blender → AgentForge (blender_bridge.py)

Мост из Blender в игровой движок **AgentForge**: экспортирует `.blend`-сцену в
формат движка — `scene.afscene` (JSON-сцена) плюс по одному `.obj`-файлу на
каждый меш-объект. Работает headless, GUI не требует, поддерживает Blender
3.x и 4.x (два API OBJ-экспорта, выбор автоматический через try/except).

## Установка

Модуль — один файл, ставить ничего не нужно:

- **по полному пути** (рекомендуется): просто вызывай его из Blender абсолютным
  путём —
  `blender --background /path/scene.blend --python /path/to/blender_bridge.py -- ...`
- **рядом со сценой**: скопируй `blender_bridge.py` в каталог с `.blend` и
  запускай с относительным именем:
  `blender --background scene.blend --python blender_bridge.py -- ...`

Требуется Blender ≥ 3.x с Python-доступом к `bpy` (любая стандартная сборка).
Модуль запускается только внутри Blender; вне его завершается с ошибкой.

## Запуск

```
blender --background scene.blend --python blender_bridge.py -- --out /path/out_dir
```

- Всё, что после `--`, — аргументы моста (аргументы самого Blender идут до `--`).
- `--out DIR` — обязательный каталог вывода (создаётся вместе с `DIR/assets/`).
- `--scene-name NAME` — необязательное имя сцены без расширения (по умолчанию
  `scene`, файл будет `DIR/scene.afscene`).
- `-h` / `--help` — краткая подсказка.

Пример:

```
blender --background arena.blend --python /home/user/agentforge/scripts/blender/blender_bridge.py -- --out /home/user/export --scene-name arena
```

## Что получается на выходе

```
out_dir/
├── scene.afscene          # сцена движка: {"format":"agentforge-scene","version":1,...}
└── assets/
    ├── Cube.obj           # геометрия одного меш-объекта (локальные координаты)
    ├── Cube.mtl           # Kd-цвет первого материала (если экспортёр пишет mtl)
    └── ...
```

Соответствие объектов Blender и нод AgentForge:

| Blender | AgentForge |
|---|---|
| MESH | `MeshInstance3D`: `mesh {file:"assets/<Имя>.obj"}`, `position`, `rotation_euler_deg` + точный `rotation_quat_xyzw`, `scale`, `color` (Principled Base Color → `mat.diffuse_color` → fallback `[0.8,0.8,0.8]`), `visible`, `static_body=true`, `mesh_class` = имя коллекции объекта (или имя объекта) |
| CAMERA | `Camera3D`: `fov_deg = degrees(2*atan(18/lens))`, `near_z`/`far_z` из clip_start/clip_end; `active=true` у первой камеры, у остальных `false` |
| LIGHT (SUN) | `Light3D`: `color` из света, `intensity = energy/1000` с clamp [0..10]; направление = поворот ноды (и там, и там луч идёт вдоль локального −Z) |
| EMPTY и прочие объекты с детьми | `Node3D` (держатель иерархии) |
| Анимация location / rotation_euler / rotation_quaternion / scale | `Animator` (ребёнок scene root), целевой объект — ребёнок Animator'а, `target_path` = имя объекта |

Геометрия пишется в **локальных координатах** (на время экспорта трансформ
объекта обнуляется и восстанавливается), поэтому положение объекта живёт в
ноде сцены, а не в вершинах OBJ. Иерархия Blender (parent-цепочки) переносится
как вложенность нод; у анимированных объектов трансформ запекается из мирового.

Каждый объект обрабатывается в своём try/except: ошибка одного объекта не
останавливает экспорт, предупреждения попадают в `warnings` и в stdout. В конце
в stdout печатается JSON-сводка (число файлов/нод, warnings).

## Как загрузить результат в движок

Пути к мешам в сцене относительные (`assets/<Имя>.obj`), поэтому запускай
`afengine` из каталога, где лежит `scene.afscene` и `assets/`:

```
cd /path/out_dir
afengine serve
```

дальше по протоколу (stdin → stdout, по строке JSON):

```
{"cmd":"scene.load","path":"scene.afscene"}
{"cmd":"scene.tree"}
{"cmd":"frame.capture","width":640,"height":360,"save":"frame.ppm"}
```

Подробности протокола — `docs/AGENT_PROTOCOL.md` (от корня репозитория), формат
сцены — `docs/SCENE_FORMAT.md`.

## Ограничения

- **Материалы упрощаются до одного Kd-цвета**: из Principled BSDF берётся
  непосвязанный вход `Base Color`, иначе viewport-цвет материала. Текстуры,
  металличность, roughness и пр. не переносятся (движок красит меш цветом ноды).
- **Анимации — только loc/rot/scale keyframes**: кривые (модификаторы fcurve,
  констрейнты, драйверы, шейп-ключи, NLA-слои) не поддерживаются; берутся
  keyframe_points каналов `location`, `scale`, `rotation_euler` (или
  `rotation_quaternion` при rotation_mode QUATERNION). `rotation_mode`
  AXIS_ANGLE не поддерживается (берётся статичный поворот с предупреждением).
- **Одна анимация = один Animator на объект**; объект перемещается под свою
  Animator-ноду (движок ищет `target_path` среди детей Animator), мировой
  трансформ сохраняется.
- **Свет**: только SUN (движок имеет один направленный свет); POINT/AREA/SPOT
  пропускаются с предупреждением.
- **Оси**: Blender Z-up, движок Y-up — координаты копируются как есть, без
  конверсии. Импортированная сцена согласована внутри себя, но «стоит» боком к
  земле движка (плоскость y=0): при необходимости поверни камеру/сцену в
  Blender до экспорта.
- **FOV**: формула даёт горизонтальный FOV (сенсор 36 мм), движок трактует
  `fov_deg` как вертикальный — при переносе камера может видеть чуть иначе.
- **Версия движка 0.1.0**: `scene.load` применяет из файла props и сахарные
  трансформы; поле `mesh` у нод из файла текущая версия игнорирует — геометрия
  прикрепляется командой `node.add`/`node.set` со спецификацией
  `{"mesh":{"file":"assets/<Имя>.obj"}}` (пример в `docs/AGENT_PROTOCOL.md`).
  Сцена от моста готова к этому: имена путей и props уже разложены по нодам.
- OBJ-экспортёр Blender 4.x может не писать `.mtl` для простых материалов —
  это нормально: цвет уже лежит в `color` ноды.
