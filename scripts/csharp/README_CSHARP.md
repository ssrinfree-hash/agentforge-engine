# AgentForge C# SDK

C# SDK для игрового движка **AgentForge**: обёртка над строковым JSON-протоколом
бинарника `afengine serve` (движок читает stdin построчно — каждая строка запрос
`{"cmd":"...", ...}` — и на каждый запрос выводит в stdout ровно одну строку
ответа `{"ok":true/false, ...}`, запрос и ответ завершаются `\n`).

## Состав

| Файл | Назначение |
| --- | --- |
| `AgentForge.cs` | Сам SDK: класс `AgentBridge : IDisposable`, типы `FrameReport`, `FrameObject`, `RayHit`, `AssetSearchResult`, `AssetItem`, `AgentForgeException` (namespace `AgentForgeSdk`) |
| `OrbRun.cs` | Пример «игры» на SDK: игрок-капсула собирает монеты на арене (namespace `AgentForgeSdk.Examples`) |
| `build.sh` | Создаёт минимальный `AgentForge.csproj` (heredoc, net6.0) и вызывает `dotnet build` |
| `AgentForge.csproj` | Генерируется `build.sh` при первом запуске (не хранится в репозитории) |

## Требования и установка

- **.NET SDK 6.0+** (`System.Text.Json` входит в состав runtime, внешние NuGet-пакеты не нужны).
- Бинарник движка `afengine` (запускается в режиме `serve`): либо в `PATH`, либо путь к нему передаётся аргументом.

Установка .NET SDK:

```bash
# Ubuntu/Debian
sudo apt-get update && sudo apt-get install -y dotnet-sdk-6.0

# универсально (официальный скрипт)
curl -fsSL https://dot.net/v1/dotnet-install.sh -o dotnet-install.sh
bash dotnet-install.sh --channel 6.0
export PATH="$HOME/.dotnet:$PATH"
```

Сборка:

```bash
bash build.sh        # создаст AgentForge.csproj (если нет) и выполнит dotnet build -c Release
```

> **Mono не поддерживается сборкой по умолчанию:** `System.Text.Json` не входит
> в mono/mcs. Собрать под mono можно только добавив NuGet-пакет
> `System.Text.Json` и сменив таргет на `netstandard2.0` (в коде нет привязок к
> .NET 6 API, кроме `ProcessStartInfo.StandardInputEncoding` — доступен с
> .NET Core 3.0). `build.sh` целенаправленно проверяет только `dotnet`.

## Быстрый старт

```csharp
using AgentForgeSdk;

using var engine = new AgentBridge("afengine");   // запускает процесс движка
engine.SceneNew();
engine.AddBox("root", "Crate", new[] { 2.0, 1.0, 2.0 },
              new[] { 0.0, 0.5, 0.0 }, new[] { 0.8, 0.4, 0.1 });
engine.SimStep(60);
var hit = engine.Raycast(new[] { 0.0, 1.0, 0.0 }, new[] { 0.0, -1.0, 0.0 });
Console.WriteLine($"Луч попал в {hit.Path} на дистанции {hit.Dist}");
var frame = engine.FrameCapture(640, 360);        // типизированный снимок кадра
Console.WriteLine($"Объектов в кадре: {frame.Objects.Count}");
engine.Quit();                                    // вежливое завершение движка
```

Если движок вернул `{"ok":false, ...}`, любой метод бросает
`AgentForgeException` с полем `Error` (содержимое поля `"error"` ответа) и
`Response` (полный ответ). Ответы типа `JsonElement` — это клоны, владеющие
своей памятью: их можно хранить без оглядки на срок жизни внутреннего документа.

## Запуск примера OrbRun

```bash
cd scripts/csharp
bash build.sh
dotnet run --project AgentForge.csproj -c Release -- afengine
# 'afengine' после -- — путь к бинарнику движка; можно указать полный путь
```

Пример строит арену (плоскость 20×20, 10 монет-сфер радиуса 0.3 с
`mesh_class:"coin"` по кругу радиуса 5, капсула-игрок с `body:true` в центре,
`Camera3D` сверху под 45°, `Light3D`), 600 кадров ведёт игрока к ближайшей
монете (`node.set` position/velocity), при дистанции < 0.6 удаляет монету
(`node.remove`) и увеличивает счёт (`scores.set`), а в конце снимает
`frame.capture` и печатает сводку: собранные монеты, кадры, сим-время,
объекты в кадре, треугольники, счёт движка.

## Методы AgentBridge

| Метод | Команда протокола | Возвращает | Описание |
| --- | --- | --- | --- |
| `SendRaw(string json)` | любой | `JsonDocument` | Сырой запрос/ответ (ответ надо Dispose); потокобезопасно |
| `Ping()` | `ping` | `JsonElement` (корень: `engine`, `version`, `time`) | Проверка связи |
| `Schema()` | `schema` | `JsonElement` (`schema`) | Схема движка: типы узлов, команды, примеры |
| `SceneNew()` | `scene.new` | `JsonElement` (`scene`) | Новая пустая сцена |
| `SceneLoad(path)` | `scene.load` | `JsonElement` (корень) | Загрузка сцены из файла |
| `SceneSave(path)` | `scene.save` | `JsonElement` (корень) | Сохранение сцены в файл |
| `SceneTree()` | `scene.tree` | `JsonElement` (`scene`) | Дерево сцены (имя, тип, дети) |
| `NodeAdd(parent, node)` | `node.add` | `string` (путь узла) | Добавить узел по произвольному описанию |
| `AddBox(parent, name, size, position?, color?, staticBody?)` | `node.add` | `string` | Куб (`MeshInstance3D`, primitive `box`) |
| `AddPlane(parent, name, size, position?, color?, staticBody?)` | `node.add` | `string` | Плоскость (primitive `plane`) |
| `AddSphere(parent, name, radius, position?, color?, staticBody?, meshClass?)` | `node.add` | `string` | Сфера (primitive `sphere`) |
| `AddCapsule(parent, name, radius, height, position?, color?, body?, staticBody?)` | `node.add` | `string` | Капсула (primitive `capsule`) |
| `AddCamera(parent, name, position?, eulerDeg?)` | `node.add` | `string` | Камера (`Camera3D`) |
| `AddLight(parent, name, eulerDeg?, position?)` | `node.add` | `string` | Свет (`Light3D`) |
| `AddAnimator(parent, name, position?, props?)` | `node.add` | `string` | Аниматор (`Animator`) |
| `NodeSet(path, props)` | `node.set` | `JsonElement` (`node`) | Изменить свойства узла |
| `NodeGet(path)` | `node.get` | `JsonElement` (`node`) | Состояние узла |
| `NodeRemove(path)` | `node.remove` | — | Удалить узел |
| `SimStep(frames = 1, dt = 1/60)` | `sim.step` | `(int Frames, double Time, JsonElement[] Events, Dictionary<string,double> Scores)` | Шаг симуляции |
| `Raycast(origin, dir, maxDist = 100)` | `raycast` | `RayHit` (`Hit`, `Path`, `Dist`, `Point`) | Бросить луч |
| `FrameCapture(width = 640, height = 360, save = null)` | `frame.capture` | `FrameReport` | Снимок кадра (см. ниже) |
| `AssetSearch(query, topK = 10)` | `asset.search` | `AssetSearchResult` | Поиск в каталоге ассетов |
| `ScoresSet(scores)` | `scores.set` | — | Установить счёт |
| `PluginLoad(path)` | `plugin.load` | `JsonElement` (`plugin`) | Загрузить плагин |
| `PluginCall(event)` | `plugin.call` | `JsonElement` (`plugin`) | Вызвать событие плагина |
| `Quit()` | `quit` | — | Завершить движок и освободить ресурсы |
| `Dispose()` | — | — | Закрыть потоки, при необходимости Kill процесса |

`FrameReport` содержит: `Frame`, `SimTime`, `Resolution`, `Camera`,
`Objects` (список `FrameObject`: `Name`, `Path`, `Type`, `Class`,
`ScreenBBox[4]`, `DepthMin`/`DepthMax` из `depth_range`, `Visible`, `WorldPos`,
`Color`, `Tris`), статистику (`DrawnTris`, `RenderMs`, `PixelHash`) и `Scores`.

Типизация запросов: в `NodeAdd`/`NodeSet`/`PluginCall` можно передавать
анонимные типы, `Dictionary<string, object?>` или `JsonElement`; строка
трактуется как готовый JSON. Все методы синхронные и потокобезопасные: запись
запроса и чтение ровно одной строки ответа выполняются под общей блокировкой.

## Соответствие протоколу движка

SDK сверён с реализацией протокола в C++ движке (`src/agent.cpp` и
обработчики команд в `src/scene.cpp`, `src/render.cpp`, `src/assets.cpp`);
покрыты все команды, доступные в `afengine serve`:

| Команда | Метод SDK | Статус |
| --- | --- | --- |
| `ping` | `Ping()` | реализовано |
| `schema` | `Schema()` | реализовано |
| `scene.new` | `SceneNew()` | реализовано |
| `scene.load` | `SceneLoad(path)` | реализовано |
| `scene.save` | `SceneSave(path)` | реализовано |
| `scene.tree` | `SceneTree()` | реализовано |
| `node.add` | `NodeAdd(parent, node)`, `AddBox`, `AddPlane`, `AddSphere`, `AddCapsule`, `AddCamera`, `AddLight`, `AddAnimator` | реализовано |
| `node.set` | `NodeSet(path, props)` | реализовано |
| `node.get` | `NodeGet(path)` | реализовано |
| `node.remove` | `NodeRemove(path)` | реализовано |
| `sim.step` | `SimStep(frames, dt)` | реализовано |
| `raycast` | `Raycast(origin, dir, maxDist)` | реализовано |
| `frame.capture` | `FrameCapture(width, height, save)` | реализовано |
| `asset.search` | `AssetSearch(query, topK)` | реализовано |
| `scores.set` | `ScoresSet(scores)` | реализовано |
| `plugin.load` | `PluginLoad(path)` | реализовано |
| `plugin.call` | `PluginCall(event)` | реализовано |
| `quit` | `Quit()` | реализовано |

Поля ответов разобраны в типизированные структуры (`frame.capture` →
`FrameReport`/`FrameObject` с `screen_bbox`, `depth_range`, `world_pos`,
`stats.drawn_tris/render_ms/pixel_hash`; `raycast` → `RayHit` с `hit`, `path`,
`dist`, `point`; `asset.search` → `AssetSearchResult` с `results`,
`n_catalog`, `model`). Все коды симуляции (`sim.step`) и чтение состояния
(`node.get`) читают поля `frames`, `time`, `events`, `scores`, `position`,
`rotation_quat_xyzw`, `scale`, `children` напрямую из ответа движка.

## Примечание о среде разработки

В песочнице, где разрабатывался SDK, **mono и .NET SDK отсутствовали**, поэтому
компиляция не выполнялась — SDK поставляется как исходники. Корректность
обеспечена строгой сверкой каждого метода с протоколом и исходниками движка
(таблица выше). После установки .NET SDK 6.0+ достаточно выполнить
`bash build.sh`; сборка была целевой средой проверки (net6.0, LangVersion
latest, Nullable enable).
