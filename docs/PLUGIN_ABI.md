# AgentForge — ABI плагинов игровой логики (PLUGIN_ABI)

Игровая логика в AgentForge подключается как разделяемая библиотека (`.so`)
через `dlopen` с C ABI. Плагин получает от движка структуру `AfApi` — набор
функциональных указателей для работы со сценой и счётчиками — и вызывается
движком по жизненному циклу. Загрузка — команда протокола `plugin.load` (см.
`docs/AGENT_PROTOCOL.md`), пример минимального плагина — раздел 4.

## 1. Структура AfApi

Каноническое объявление — `include/af/agent.h` (движок, C++17):

```cpp
struct AfApi {
  void* session;                                              // внутр. контекст движка, не трогать
  int   (*node_add)(void* session, const char* parent_path, const char* node_json);
  int   (*node_set)(void* session, const char* path, const char* props_json);
  const char* (*node_get)(void* session, const char* path);   // буфер движка — копируй сразу
  double (*score_get)(void* session, const char* key);
  void  (*score_set)(void* session, const char* key, double v);
  void  (*log)(const char* msg);
  double (*time)(void* session);
  Json  (*last_events)(void* session);   // движковый Json — см. примечание ниже
};
```

Каждая функция принимает `session` первым аргументом (передавай
`api->session` как есть). Семантика:

| функция | сигнатура | поведение | возврат |
|---|---|---|---|
| `node_add` | `(void*, const char* parent_path, const char* node_json)` | выполняет эквивалент команды `node.add`: `node_json` — JSON-объект ноды (`{"type":...,"name":...,"props":{...}}`), `parent_path` — путь родителя (`"root"` или путь без префикса `root/`) | `0` — успех; `-1` — node_json не парсится; `-2` — движок вернул `ok:false` |
| `node_set` | `(void*, const char* path, const char* props_json)` | эквивалент `node.set`: `props_json` — объект свойств; `path` без префикса `root/` | `0` / `-1` (плохой JSON) / `-2` (ошибка) |
| `node_get` | `(void*, const char* path) -> const char*` | эквивалент `node.get`: возвращает JSON-строку — полное self-описание ноды (props + `children`). Указатель на **внутренний буфер движка**: валиден до следующего вызова любого ABI-метода — сразу копируй к себе | строка JSON |
| `score_get` | `(void*, const char* key) -> double` | счётчик игрового состояния; отсутствующий ключ = `0.0` | число |
| `score_set` | `(void*, const char* key, double v)` | устанавливает счётчик (появляется в `sim.step` и FrameReport) | — |
| `log` | `(const char* msg)` | строка в stderr движка с префиксом `[plugin]` и в накопленный лог событий сессии | — |
| `time` | `(void*) -> double` | текущее время симуляции (сек) | число |
| `last_events` | `(void*)` | накопленный лог событий сессии | движковый `Json` |

Примечание про `last_events`: поле типизировано внутренним классом движка
`af::Json` (`include/af/json.h`) — по сути json-строка/объект движка: массив
событий вида `{"event":"log","msg":"..."}` (всё, что плагин напечатал через
`log`). Плагины, собираемые независимо от движка, объявляйте этот слот как
`void* (*)(void*)` и **не вызывайте его** — движок заполнит слот своим
указателем (размер совместим), а свои события плагин копит сам. Поле полезно
компонентам, собранным вместе с заголовками движка.

## 2. Обязательные и опциональные символы

Плагин экспортирует (внешнее связывание C, `extern "C"`, unmangled):

| символ | сигнатура | обязателен | смысл |
|---|---|---|---|
| `af_plugin_init` | `int (const AfApi*)` | да | инициализация: сохранить `AfApi`, создать свои ноды. Возврат `0` = успех; любой другой код → `plugin.load` вернёт `ok:false` с текстом `af_plugin_init returned <N>` |
| `af_plugin_update` | `void (float dt, double time)` | да | кадр логики: вызывается движком **внутри каждого кадра `sim.step`** (`dt` — шаг, `time` — время симуляции) |
| `af_plugin_event` | `const char* (const char* json)` | нет | обработчик разовых событий (`plugin.call`): на вход JSON-строка запроса, на выход — JSON-строка ответа или `nullptr`. Возвращаемый указатель должен указывать на **статический буфер плагина** (валиден до следующего вызова); движок попытается распарсить его как JSON — при успехе ответ кладётся в поле `plugin` ответа, иначе в `plugin_raw` |

Движок ищет символы через `dlsym`; без `af_plugin_init`/`af_plugin_update`
загрузка отклоняется (`missing af_plugin_init/af_plugin_update`).

## 3. Жизненный цикл

```
plugin.load {"path":"./game_logic.so"}
    └─ dlopen(RTLD_NOW) → dlsym → заполнение AfApi
       (session = активная сессия движка) → af_plugin_init(api)
sim.step {"frames":N,"dt":...}
    └─ для каждого кадра: обновление Animator'ов → физика → af_plugin_update(dt, time)
plugin.call {"event":{...}}
    └─ af_plugin_event(json) → ответ в поле plugin/plugin_raw
plugin.load (повторный) / quit
    └─ предыдущая библиотека закрывается (dlclose) / процесс завершается
```

Правила и ограничения:

- Все вызовы движка — только из колбэков (`af_plugin_init`, `af_plugin_update`,
  `af_plugin_event`), в одном потоке с движком. Не кешируй `const char*` из
  `node_get` дольше одного вызова.
- `plugin.load` после сцены: ноды, созданные плагином через `node_add`, живут в
  текущей сцене; `scene.new` сцену пересоздаёт — переинициализации нет,
  учти это в логике.
- `log()` — единственный канал печати плагина (stderr `[plugin] ...` + лог
  сессии); не пиши в stdout движка — он занят ответами протокола.
- Счётчики `score_*` видны агенту в каждом ответе `sim.step` и
  `frame.capture` — это канал состояния «плагин → агент».
- Плагин собирается независимо от движка: движку не нужны его заголовки,
  только символы. Свой `AfApi` плагин объявляет сам (копией структуры выше) —
  **порядок полей менять нельзя**.

## 4. Минимальный пример: счётчик вращения ноды

Плагин создаёт в корне ноду `Spinner` и вращает её на 90°/сек; каждый полный
оборот увеличивает счётчик `spins` (агент видит его в `sim.step`).

```cpp
// game_logic.cpp — минимальный плагин AgentForge (C ABI).
#include <cmath>
#include <cstdio>

struct AfApi {
  void* session;
  int   (*node_add)(void*, const char*, const char*);
  int   (*node_set)(void*, const char*, const char*);
  const char* (*node_get)(void*, const char*);
  double (*score_get)(void*, const char*);
  void  (*score_set)(void*, const char*, double);
  void  (*log)(const char*);
  double (*time)(void*);
  void* (*last_events)(void*);   // слот движкового Json; здесь не используется
};

static const AfApi* api = nullptr;

extern "C" int af_plugin_init(const AfApi* a) {
  api = a;
  if (!api || !api->node_add || !api->node_set) return -1;
  api->node_add(api->session, "root",
                "{\"type\":\"Node3D\",\"name\":\"Spinner\",\"position\":[0,1,-4]}");
  api->log("spinner plugin initialized");
  return 0;
}

extern "C" void af_plugin_update(float dt, double time) {
  (void)dt;
  if (!api) return;
  double angle = std::fmod(time * 90.0, 360.0);          // 90° в секунду
  static double prev_angle = 0.0;
  if (angle < prev_angle) {                              // заворот через 360° → оборот
    api->score_set(api->session, "spins",
                   api->score_get(api->session, "spins") + 1.0);
  }
  prev_angle = angle;
  char props[96];
  std::snprintf(props, sizeof props, "{\"rotation_euler_deg\":[0,%.2f,0]}", angle);
  api->node_set(api->session, "Spinner", props);
}

extern "C" const char* af_plugin_event(const char* json) {  // опционально
  (void)json;
  static const char* resp = "{\"plugin\":\"spinner\",\"ok\":true}";
  return resp;
}
```

Подсчёт оборотов сделан через статический `prev_angle`: когда
`std::fmod` заворачивает угол через 360°, фиксируется полный оборот.

## 5. Сборка и подключение

```
g++ -std=c++17 -O2 -fPIC -shared game.cpp -o game.so -ldl
```

- `-fPIC -shared` — разделяемая библиотека; `-ldl` — dlopen/dlsym (на части
  систем уже в libc, флаг безвреден); никаких заголовков движка не требуется.
- В репозитории есть цель `make plugin` — собирает `game/game_logic.cpp` в
  `build/game_logic.so` той же командой (`CXXFLAGS` из Makefile).
- Подключение (плагин вращает ноду, агент смотрит на результат):

```
{"cmd":"plugin.load","path":"./game_logic.so"}
{"ok":true}
{"cmd":"sim.step","frames":120}
{"events":[],"frame":120,"frames":120,"ok":true,"scores":{"spins":3.0},"time":2.0}
{"cmd":"plugin.call","event":{"action":"status"}}
{"ok":true,"plugin":{"ok":true,"plugin":"spinner"}}
```

- Проверить сборку можно так: `nm -D game.so | grep af_plugin` — должны быть
  `af_plugin_init` и `af_plugin_update` (T = текстовый символ).
