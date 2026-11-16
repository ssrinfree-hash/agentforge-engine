// ---------------------------------------------------------------------------
// AgentForge.cs — C# SDK для игрового движка AgentForge.
//
// Протокол (реализован в C++-движке `afengine serve`):
//   - движок читает stdin ПОСТРОЧНО, каждая строка — JSON-запрос {"cmd":"...", ...};
//   - на каждый запрос в stdout выводится ровно ОДНА строка JSON-ответа
//     {"ok":true/false, ...};
//   - запрос и ответ завершаются символом '\n'.
//
// Требования: .NET 6+ (System.Text.Json входит в состав runtime),
// внешние NuGet-пакеты не нужны.
//
// Потокобезопасность: каждый запрос-ответ выполняется под общей блокировкой,
// поэтому методы AgentBridge можно звать из нескольких потоков.
// ---------------------------------------------------------------------------
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AgentForgeSdk
{
    /// <summary>
    /// Ошибка обмена с движком: движок вернул {"ok":false, ...}, прислал
    /// некорректный JSON, оборвался поток stdin/stdout либо процесс не запустился.
    /// </summary>
    public class AgentForgeException : Exception
    {
        /// <summary>Значение поля "error" из ответа движка (если оно присутствовало).</summary>
        public string? Error { get; }

        /// <summary>
        /// Полный ответ движка (или его поле "error") в виде клона, владеющего
        /// собственной памятью; безопасен после освобождения исходного документа.
        /// </summary>
        public JsonElement? Response { get; }

        /// <summary>Создаёт исключение обмена с движком.</summary>
        /// <param name="message">Человекочитаемое описание ошибки.</param>
        /// <param name="error">Поле "error" из ответа движка (если было).</param>
        /// <param name="response">Ответ движка целиком (клон) или null.</param>
        /// <param name="inner">Внутреннее исключение (например JsonException).</param>
        public AgentForgeException(string message, string? error = null,
            JsonElement? response = null, Exception? inner = null)
            : base(message, inner)
        {
            Error = error;
            Response = response;
        }
    }

    /// <summary>Объект сцены, попавший в итоговый кадр (frame.capture).</summary>
    public class FrameObject
    {
        /// <summary>Имя узла.</summary>
        public string Name { get; set; } = "";

        /// <summary>Путь узла в дереве сцены (например "root/Crate").</summary>
        public string Path { get; set; } = "";

        /// <summary>Тип узла (MeshInstance3D, Camera3D и т.п.).</summary>
        public string Type { get; set; } = "";

        /// <summary>Класс меша (mesh_class), если задан ("crate", "coin" и т.п.).</summary>
        public string? Class { get; set; }

        /// <summary>Экранный габарит [x0, y0, x1, y1] в пикселях.</summary>
        public double[]? ScreenBBox { get; set; }

        /// <summary>Ближний край диапазона глубины (depth_range[0]).</summary>
        public double? DepthMin { get; set; }

        /// <summary>Дальний край диапазона глубины (depth_range[1]).</summary>
        public double? DepthMax { get; set; }

        /// <summary>Виден ли объект камере.</summary>
        public bool Visible { get; set; }

        /// <summary>Мировая позиция объекта [x, y, z].</summary>
        public double[]? WorldPos { get; set; }

        /// <summary>Цвет объекта [r, g, b] (если задан).</summary>
        public double[]? Color { get; set; }

        /// <summary>Число треугольников меша объекта.</summary>
        public int Tris { get; set; }
    }

    /// <summary>Результат frame.capture — типизированный снимок кадра.</summary>
    public class FrameReport
    {
        /// <summary>Номер кадра симуляции.</summary>
        public long Frame { get; set; }

        /// <summary>Симуляционное время, с.</summary>
        public double SimTime { get; set; }

        /// <summary>Разрешение кадра [ширина, высота] в пикселях.</summary>
        public int[]? Resolution { get; set; }

        /// <summary>Описание камеры (сырой JSON-объект "camera").</summary>
        public JsonElement? Camera { get; set; }

        /// <summary>Объекты, попавшие в кадр.</summary>
        public List<FrameObject> Objects { get; } = new List<FrameObject>();

        /// <summary>Нарисовано треугольников (stats.drawn_tris).</summary>
        public long DrawnTris { get; set; }

        /// <summary>Время рендера, мс (stats.render_ms).</summary>
        public double RenderMs { get; set; }

        /// <summary>Хэш пикселей кадра (stats.pixel_hash).</summary>
        public string? PixelHash { get; set; }

        /// <summary>Счёт движка на момент снимка.</summary>
        public Dictionary<string, double>? Scores { get; set; }
    }

    /// <summary>Результат raycast.</summary>
    public class RayHit
    {
        /// <summary>Было ли попадание.</summary>
        public bool Hit { get; set; }

        /// <summary>Путь узла, в который попал луч (если hit=true).</summary>
        public string? Path { get; set; }

        /// <summary>Дистанция до точки попадания.</summary>
        public double Dist { get; set; }

        /// <summary>Точка попадания [x, y, z].</summary>
        public double[]? Point { get; set; }
    }

    /// <summary>Один результат поиска из каталога ассетов.</summary>
    public class AssetItem
    {
        /// <summary>Идентификатор ассета (например "chair_0001").</summary>
        public string Id { get; set; } = "";

        /// <summary>Класс ассета (например "chair").</summary>
        public string? Class { get; set; }

        /// <summary>Человекочитаемое имя ассета.</summary>
        public string? Name { get; set; }

        /// <summary>Число вершин.</summary>
        public int Verts { get; set; }

        /// <summary>Число граней.</summary>
        public int Faces { get; set; }

        /// <summary>Оценка совпадения с классом (class_score).</summary>
        public double ClassScore { get; set; }
    }

    /// <summary>Результат asset.search.</summary>
    public class AssetSearchResult
    {
        /// <summary>Запрос, как он был отправлен.</summary>
        public string Query { get; set; } = "";

        /// <summary>Найденные ассеты (не более top_k).</summary>
        public List<AssetItem> Results { get; } = new List<AssetItem>();

        /// <summary>Общий размер каталога (n_catalog).</summary>
        public long NCatalog { get; set; }

        /// <summary>Модель поиска ("hash-ngram-softmax" или "substring-fallback").</summary>
        public string? Model { get; set; }
    }

    /// <summary>
    /// Мост к движку AgentForge: запускает процесс движка и обменивается с ним
    /// JSON-строками через stdin/stdout — по одной строке на запрос и на ответ.
    /// Все методы синхронные; каждый пишет одну строку запроса и читает одну
    /// строку ответа под блокировкой (потокобезопасно).
    /// </summary>
    public class AgentBridge : IDisposable
    {
        private readonly Process _process;
        private readonly StreamWriter _stdin;
        private readonly StreamReader _stdout;
        private readonly object _ioLock = new object();
        private bool _disposed;

        private static readonly JsonSerializerOptions JsonOpts = new JsonSerializerOptions
        {
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        };

        private static readonly Dictionary<string, object?> NoArgs = new Dictionary<string, object?>();

        /// <summary>Путь к исполняемому файлу движка, переданный в конструктор.</summary>
        public string EnginePath { get; }

        /// <summary>Аргументы командной строки движка, переданные в конструктор.</summary>
        public string EngineArgs { get; }

        /// <summary>Завершён ли процесс движка (или уже освобождён).</summary>
        public bool HasExited
        {
            get { try { return _process.HasExited; } catch { return true; } }
        }

        /// <summary>
        /// Запускает процесс движка и открывает каналы stdin/stdout (UTF-8).
        /// </summary>
        /// <param name="enginePath">Путь к бинарнику движка, по умолчанию "afengine".</param>
        /// <param name="args">Дополнительные аргументы командной строки движка.</param>
        /// <exception cref="AgentForgeException">Если процесс запустить не удалось.</exception>
        public AgentBridge(string enginePath = "afengine", string args = "")
        {
            EnginePath = enginePath ?? throw new ArgumentNullException(nameof(enginePath));
            EngineArgs = args ?? string.Empty;

            var psi = new ProcessStartInfo
            {
                FileName = EnginePath,
                Arguments = EngineArgs,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = false,
                StandardInputEncoding = new UTF8Encoding(false), // UTF-8 без BOM
                StandardOutputEncoding = Encoding.UTF8,
            };

            _process = Process.Start(psi)
                ?? throw new AgentForgeException($"Не удалось запустить процесс движка '{EnginePath}'.");

            _stdin = _process.StandardInput;
            _stdin.AutoFlush = true;
            _stdin.NewLine = "\n"; // протокол требует '\n', а не Environment.NewLine
            _stdout = _process.StandardOutput;
        }

        // ------------------------------------------------------------------
        // Низкоуровневый обмен
        // ------------------------------------------------------------------

        /// <summary>
        /// Отправляет сырую JSON-строку движку и возвращает разобранный ответ.
        /// Запись и чтение выполняются под блокировкой; на каждый запрос читается
        /// ровно одна строка ответа. Вызывающий код обязан освободить возвращённый
        /// JsonDocument (using / Dispose). Если ok=false — бросает
        /// <see cref="AgentForgeException"/> с полем "error" из ответа.
        /// </summary>
        /// <param name="json">Полная строка запроса, например {"cmd":"ping"}.</param>
        public JsonDocument SendRaw(string json)
        {
            if (json is null) throw new ArgumentNullException(nameof(json));
            ThrowIfDisposed();

            lock (_ioLock)
            {
                string? line;
                try
                {
                    _stdin.WriteLine(json);
                    line = _stdout.ReadLine();
                }
                catch (IOException ex)
                {
                    throw new AgentForgeException(
                        "Поток stdin/stdout движка закрыт: процесс завершился до ответа.",
                        null, null, ex);
                }

                if (line is null)
                    throw new AgentForgeException(
                        "Движок закрыл stdout, не вернув ответ (процесс завершился).");

                JsonDocument doc;
                try
                {
                    doc = JsonDocument.Parse(line);
                }
                catch (JsonException ex)
                {
                    throw new AgentForgeException(
                        $"Некорректный JSON в ответе движка: {line}",
                        null, null, ex);
                }

                if (doc.RootElement.ValueKind == JsonValueKind.Object &&
                    doc.RootElement.TryGetProperty("ok", out JsonElement okEl) &&
                    okEl.ValueKind == JsonValueKind.False)
                {
                    // Клонируем корень целиком: клон владеет своей памятью,
                    // поэтому исключение безопасно переживает Dispose документа.
                    string? error = GetString(doc.RootElement, "error");
                    JsonElement payload = doc.RootElement.Clone();
                    doc.Dispose();
                    throw new AgentForgeException(
                        error ?? "Движок вернул ok=false без описания ошибки.",
                        error, payload);
                }

                return doc;
            }
        }

        // ------------------------------------------------------------------
        // Служебные команды
        // ------------------------------------------------------------------

        /// <summary>
        /// Проверка связи: {"cmd":"ping"}. Возвращает весь корневой ответ
        /// (поля "engine", "version", "time") в виде клона.
        /// </summary>
        public JsonElement Ping()
        {
            using var doc = Call("ping", NoArgs);
            return doc.RootElement.Clone();
        }

        /// <summary>Схема движка: {"cmd":"schema"} → поле "schema" (типы узлов, команды, примеры).</summary>
        public JsonElement Schema()
        {
            using var doc = Call("schema", NoArgs);
            return CloneProp(doc, "schema");
        }

        // ------------------------------------------------------------------
        // Сцена
        // ------------------------------------------------------------------

        /// <summary>Создаёт новую пустую сцену: {"cmd":"scene.new"} → поле "scene".</summary>
        public JsonElement SceneNew()
        {
            using var doc = Call("scene.new", NoArgs);
            return CloneProp(doc, "scene");
        }

        /// <summary>
        /// Загружает сцену из файла: {"cmd":"scene.load","path":"..."}.
        /// Возвращает весь корневой ответ движка.
        /// </summary>
        public JsonElement SceneLoad(string path)
        {
            var fields = new Dictionary<string, object?> { ["path"] = path };
            using var doc = Call("scene.load", fields);
            return doc.RootElement.Clone();
        }

        /// <summary>
        /// Сохраняет сцену в файл: {"cmd":"scene.save","path":"..."}.
        /// Возвращает весь корневой ответ движка.
        /// </summary>
        public JsonElement SceneSave(string path)
        {
            var fields = new Dictionary<string, object?> { ["path"] = path };
            using var doc = Call("scene.save", fields);
            return doc.RootElement.Clone();
        }

        /// <summary>Дерево сцены: {"cmd":"scene.tree"} → поле "scene" (имя, тип, дети).</summary>
        public JsonElement SceneTree()
        {
            using var doc = Call("scene.tree", NoArgs);
            return CloneProp(doc, "scene");
        }

        // ------------------------------------------------------------------
        // Узлы
        // ------------------------------------------------------------------

        /// <summary>
        /// Добавляет узел: {"cmd":"node.add","parent":...,"node":{...}}.
        /// Возвращает путь созданного узла (например "root/Crate").
        /// </summary>
        /// <param name="parent">Путь родительского узла (обычно "root").</param>
        /// <param name="node">
        /// Описание узла: анонимный тип, Dictionary&lt;string,object?&gt;, JsonElement
        /// либо строка с готовым JSON. Например:
        /// <code>
        /// new { type = "MeshInstance3D", name = "Crate",
        ///       mesh = new { primitive = "box", size = new[]{ 2.0, 1.0, 2.0 } },
        ///       props = new { position = new[]{ 0.0, 0.5, 0.0 } } }
        /// </code></param>
        public string NodeAdd(string parent, object node)
        {
            var fields = new Dictionary<string, object?>
            {
                ["parent"] = parent,
                ["node"] = AsJson(node),
            };
            using var doc = Call("node.add", fields);
            return GetString(doc.RootElement, "path") ?? "";
        }

        /// <summary>
        /// Меняет свойства узла: {"cmd":"node.set","path":...,"props":{...}}.
        /// Возвращает поле "node" — обновлённое состояние узла.
        /// </summary>
        /// <param name="path">Путь узла, например "root/Crate".</param>
        /// <param name="props">Свойства: position, rotation_euler_deg, color, visible,
        /// static_body, body, mesh_class, velocity и т.п.</param>
        public JsonElement NodeSet(string path, object props)
        {
            var fields = new Dictionary<string, object?>
            {
                ["path"] = path,
                ["props"] = AsJson(props),
            };
            using var doc = Call("node.set", fields);
            return CloneProp(doc, "node");
        }

        /// <summary>
        /// Читает состояние узла: {"cmd":"node.get","path":...} → поле "node"
        /// (name, type, position, rotation_quat_xyzw, scale, children и т.п.).
        /// </summary>
        public JsonElement NodeGet(string path)
        {
            var fields = new Dictionary<string, object?> { ["path"] = path };
            using var doc = Call("node.get", fields);
            return CloneProp(doc, "node");
        }

        /// <summary>Удаляет узел: {"cmd":"node.remove","path":...}.</summary>
        public void NodeRemove(string path)
        {
            var fields = new Dictionary<string, object?> { ["path"] = path };
            using var doc = Call("node.remove", fields);
        }

        // ------------------------------------------------------------------
        // Удобные конструкторы узлов (поверх node.add)
        // ------------------------------------------------------------------

        /// <summary>Добавляет куб: MeshInstance3D + mesh {primitive:"box", size}.</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="size">Размеры по осям [x, y, z].</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="color">Цвет [r, g, b] (null — не задавать).</param>
        /// <param name="staticBody">Создать статичное физическое тело (props.static_body).</param>
        public string AddBox(string parent, string name, double[] size,
            double[]? position = null, double[]? color = null, bool staticBody = true)
        {
            var mesh = new Dictionary<string, object?> { ["primitive"] = "box", ["size"] = size };
            return AddMeshNode(parent, name, "MeshInstance3D", mesh,
                position, color, staticBody: staticBody);
        }

        /// <summary>Добавляет плоскость: MeshInstance3D + mesh {primitive:"plane", size}.</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="size">Размеры [ширина, толщина, глубина].</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="color">Цвет [r, g, b] (null — не задавать).</param>
        /// <param name="staticBody">Создать статичное физическое тело (props.static_body).</param>
        public string AddPlane(string parent, string name, double[] size,
            double[]? position = null, double[]? color = null, bool staticBody = true)
        {
            var mesh = new Dictionary<string, object?> { ["primitive"] = "plane", ["size"] = size };
            return AddMeshNode(parent, name, "MeshInstance3D", mesh,
                position, color, staticBody: staticBody);
        }

        /// <summary>Добавляет сферу: MeshInstance3D + mesh {primitive:"sphere", radius}.</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="radius">Радиус сферы.</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="color">Цвет [r, g, b] (null — не задавать).</param>
        /// <param name="staticBody">Создать статичное физическое тело (props.static_body).</param>
        /// <param name="meshClass">Класс меша (props.mesh_class), например "coin".</param>
        public string AddSphere(string parent, string name, double radius,
            double[]? position = null, double[]? color = null,
            bool staticBody = true, string? meshClass = null)
        {
            var mesh = new Dictionary<string, object?> { ["primitive"] = "sphere", ["radius"] = radius };
            return AddMeshNode(parent, name, "MeshInstance3D", mesh,
                position, color, staticBody: staticBody, meshClass: meshClass);
        }

        /// <summary>Добавляет капсулу: MeshInstance3D + mesh {primitive:"capsule", radius, height}.</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="radius">Радиус капсулы.</param>
        /// <param name="height">Высота капсулы.</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="color">Цвет [r, g, b] (null — не задавать).</param>
        /// <param name="body">Динамическое физическое тело (props.body).</param>
        /// <param name="staticBody">Статичное физическое тело (props.static_body).</param>
        public string AddCapsule(string parent, string name, double radius, double height,
            double[]? position = null, double[]? color = null,
            bool body = false, bool staticBody = false)
        {
            var mesh = new Dictionary<string, object?>
            {
                ["primitive"] = "capsule",
                ["radius"] = radius,
                ["height"] = height,
            };
            return AddMeshNode(parent, name, "MeshInstance3D", mesh,
                position, color, staticBody: staticBody, body: body);
        }

        /// <summary>Добавляет камеру: Camera3D с позицией и углами Эйлера (в градусах).</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="eulerDeg">Поворот в градусах [pitch, yaw, roll] (null — не задавать).</param>
        public string AddCamera(string parent, string name,
            double[]? position = null, double[]? eulerDeg = null)
        {
            var props = new Dictionary<string, object?>();
            if (position != null) props["position"] = position;
            if (eulerDeg != null) props["rotation_euler_deg"] = eulerDeg;

            var node = new Dictionary<string, object?> { ["type"] = "Camera3D", ["name"] = name };
            if (props.Count > 0) node["props"] = props;
            return NodeAdd(parent, node);
        }

        /// <summary>Добавляет источник света: Light3D с углами Эйлера (в градусах).</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="eulerDeg">Поворот в градусах [pitch, yaw, roll] (null — не задавать).</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        public string AddLight(string parent, string name,
            double[]? eulerDeg = null, double[]? position = null)
        {
            var props = new Dictionary<string, object?>();
            if (eulerDeg != null) props["rotation_euler_deg"] = eulerDeg;
            if (position != null) props["position"] = position;

            var node = new Dictionary<string, object?> { ["type"] = "Light3D", ["name"] = name };
            if (props.Count > 0) node["props"] = props;
            return NodeAdd(parent, node);
        }

        /// <summary>Добавляет аниматор: Animator с позицией и дополнительными свойствами.</summary>
        /// <param name="parent">Путь родителя (обычно "root").</param>
        /// <param name="name">Имя узла.</param>
        /// <param name="position">Позиция [x, y, z] (null — не задавать).</param>
        /// <param name="props">Дополнительные свойства узла (null — не задавать).</param>
        public string AddAnimator(string parent, string name,
            double[]? position = null, Dictionary<string, object?>? props = null)
        {
            var p = new Dictionary<string, object?>();
            if (position != null) p["position"] = position;
            if (props != null)
                foreach (var kv in props) p[kv.Key] = kv.Value;

            var node = new Dictionary<string, object?> { ["type"] = "Animator", ["name"] = name };
            if (p.Count > 0) node["props"] = p;
            return NodeAdd(parent, node);
        }

        // ------------------------------------------------------------------
        // Симуляция, лучи, кадр, ассеты, счёт, плагины
        // ------------------------------------------------------------------

        /// <summary>
        /// Шаг симуляции: {"cmd":"sim.step","frames":N,"dt":T}.
        /// </summary>
        /// <param name="frames">Число кадров симуляции.</param>
        /// <param name="dt">Длительность кадра, с (по умолчанию 1/60).</param>
        /// <returns>
        /// Кортеж: Frames — число отработанных кадров, Time — симуляционное время,
        /// Events — массив событий (клоны JSON), Scores — счёт движка.
        /// </returns>
        public (int Frames, double Time, JsonElement[] Events, Dictionary<string, double> Scores)
            SimStep(int frames = 1, double dt = 1.0 / 60.0)
        {
            var fields = new Dictionary<string, object?> { ["frames"] = frames, ["dt"] = dt };
            using var doc = Call("sim.step", fields);
            var root = doc.RootElement;

            var events = Array.Empty<JsonElement>();
            var ev = GetProp(root, "events");
            if (ev is JsonElement arr && arr.ValueKind == JsonValueKind.Array)
            {
                var list = new List<JsonElement>(arr.GetArrayLength());
                foreach (var e in arr.EnumerateArray()) list.Add(e.Clone());
                events = list.ToArray();
            }

            return (
                (int)(GetInt64(root, "frames") ?? frames),
                GetDouble(root, "time") ?? 0.0,
                events,
                ReadScores(root));
        }

        /// <summary>
        /// Бросает луч: {"cmd":"raycast","origin":[...],"dir":[...],"max_dist":D}.
        /// </summary>
        /// <param name="origin">Точка старта [x, y, z].</param>
        /// <param name="dir">Направление [x, y, z].</param>
        /// <param name="maxDist">Максимальная дистанция (по умолчанию 100).</param>
        public RayHit Raycast(double[] origin, double[] dir, double maxDist = 100.0)
        {
            var fields = new Dictionary<string, object?>
            {
                ["origin"] = origin,
                ["dir"] = dir,
                ["max_dist"] = maxDist,
            };
            using var doc = Call("raycast", fields);

            var hit = new RayHit();
            if (GetProp(doc.RootElement, "raycast") is JsonElement r)
            {
                hit.Hit = GetBool(r, "hit");
                hit.Path = GetString(r, "path");
                hit.Dist = GetDouble(r, "dist") ?? 0.0;
                hit.Point = ParseDoubleArray(GetProp(r, "point"));
            }
            return hit;
        }

        /// <summary>
        /// Снимок кадра: {"cmd":"frame.capture","width":W,"height":H,"save":"..."}.
        /// </summary>
        /// <param name="width">Ширина кадра, пикс.</param>
        /// <param name="height">Высота кадра, пикс.</param>
        /// <param name="save">Путь к файлу снимка (.ppm) или null — не сохранять
        /// (поле "save" в этом случае вообще не отправляется).</param>
        public FrameReport FrameCapture(int width = 640, int height = 360, string? save = null)
        {
            var fields = new Dictionary<string, object?>
            {
                ["width"] = width,
                ["height"] = height,
                ["save"] = save,
            };
            using var doc = Call("frame.capture", fields);

            var report = new FrameReport();
            if (GetProp(doc.RootElement, "frame") is JsonElement f)
            {
                report.Frame = GetInt64(f, "frame") ?? 0;
                report.SimTime = GetDouble(f, "sim_time") ?? 0.0;

                var res = ParseDoubleArray(GetProp(f, "resolution"));
                if (res != null && res.Length >= 2)
                    report.Resolution = new[] { (int)res[0], (int)res[1] };

                var cam = GetProp(f, "camera");
                if (cam.HasValue) report.Camera = cam.Value.Clone();

                if (GetProp(f, "objects") is JsonElement objs && objs.ValueKind == JsonValueKind.Array)
                {
                    foreach (var o in objs.EnumerateArray())
                    {
                        var fo = new FrameObject
                        {
                            Name = GetString(o, "name") ?? "",
                            Path = GetString(o, "path") ?? "",
                            Type = GetString(o, "type") ?? "",
                            Class = GetString(o, "class"),
                            Visible = GetBool(o, "visible", fallback: true),
                            Tris = (int)(GetInt64(o, "tris") ?? 0),
                        };
                        fo.ScreenBBox = ParseDoubleArray(GetProp(o, "screen_bbox"));
                        fo.WorldPos = ParseDoubleArray(GetProp(o, "world_pos"));
                        fo.Color = ParseDoubleArray(GetProp(o, "color"));

                        var depth = ParseDoubleArray(GetProp(o, "depth_range"));
                        if (depth != null && depth.Length >= 1) fo.DepthMin = depth[0];
                        if (depth != null && depth.Length >= 2) fo.DepthMax = depth[1];

                        report.Objects.Add(fo);
                    }
                }

                if (GetProp(f, "stats") is JsonElement stats)
                {
                    report.DrawnTris = GetInt64(stats, "drawn_tris") ?? 0;
                    report.RenderMs = GetDouble(stats, "render_ms") ?? 0.0;
                    report.PixelHash = GetString(stats, "pixel_hash");
                }

                report.Scores = ReadScores(f);
            }
            return report;
        }

        /// <summary>
        /// Поиск в каталоге ассетов: {"cmd":"asset.search","query":...,"top_k":N}.
        /// </summary>
        /// <param name="query">Поисковый запрос, например "chair".</param>
        /// <param name="topK">Максимальное число результатов (по умолчанию 10).</param>
        public AssetSearchResult AssetSearch(string query, int topK = 10)
        {
            var fields = new Dictionary<string, object?>
            {
                ["query"] = query,
                ["top_k"] = topK,
            };
            using var doc = Call("asset.search", fields);

            var result = new AssetSearchResult();
            if (GetProp(doc.RootElement, "search") is JsonElement s)
            {
                result.Query = GetString(s, "query") ?? query;
                result.NCatalog = GetInt64(s, "n_catalog") ?? 0;
                result.Model = GetString(s, "model");

                if (GetProp(s, "results") is JsonElement arr && arr.ValueKind == JsonValueKind.Array)
                {
                    foreach (var it in arr.EnumerateArray())
                    {
                        result.Results.Add(new AssetItem
                        {
                            Id = GetString(it, "id") ?? "",
                            Class = GetString(it, "class"),
                            Name = GetString(it, "name"),
                            Verts = (int)(GetInt64(it, "verts") ?? 0),
                            Faces = (int)(GetInt64(it, "faces") ?? 0),
                            ClassScore = GetDouble(it, "class_score") ?? 0.0,
                        });
                    }
                }
            }
            return result;
        }

        /// <summary>
        /// Устанавливает счёт: {"cmd":"scores.set","scores":{"score":N, ...}}.
        /// </summary>
        /// <param name="scores">Пары имя→значение, например {"score": 5}.</param>
        public void ScoresSet(IReadOnlyDictionary<string, double> scores)
        {
            var fields = new Dictionary<string, object?> { ["scores"] = scores };
            using var doc = Call("scores.set", fields);
        }

        /// <summary>
        /// Загружает плагин: {"cmd":"plugin.load","path":"lib.so"} → поле "plugin".
        /// </summary>
        public JsonElement PluginLoad(string path)
        {
            var fields = new Dictionary<string, object?> { ["path"] = path };
            using var doc = Call("plugin.load", fields);
            return CloneProp(doc, "plugin");
        }

        /// <summary>
        /// Вызывает событие плагина: {"cmd":"plugin.call","event":{...}} → поле "plugin".
        /// </summary>
        /// <param name="event">Событие (произвольный JSON-объект).</param>
        public JsonElement PluginCall(object @event)
        {
            var fields = new Dictionary<string, object?> { ["event"] = AsJson(@event) };
            using var doc = Call("plugin.call", fields);
            return CloneProp(doc, "plugin");
        }

        /// <summary>
        /// Вежливо завершает движок командой {"cmd":"quit"} и освобождает ресурсы.
        /// Ошибки обмена игнорируются: движок закрывает потоки сразу после ответа.
        /// </summary>
        public void Quit()
        {
            try
            {
                using var doc = Call("quit", NoArgs);
            }
            catch
            {
                // Движок мог уже завершиться — это нормальная ситуация для quit.
            }
            Dispose();
        }

        /// <summary>
        /// Закрывает каналы stdin/stdout и завершает процесс движка
        /// (если тот ещё работает — сначала Kill после таймаута).
        /// </summary>
        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;

            try { _stdin.Dispose(); } catch { }
            try { _stdout.Dispose(); } catch { }
            try
            {
                if (!_process.WaitForExit(500))
                {
                    try { _process.Kill(); } catch { }
                    _process.WaitForExit(1000);
                }
            }
            catch { }
            _process.Dispose();
        }

        // ------------------------------------------------------------------
        // Внутренние помощники
        // ------------------------------------------------------------------

        /// <summary>Формирует запрос {"cmd":..., ...fields} (null-поля пропускаются) и отправляет его.</summary>
        private JsonDocument Call(string cmd, IReadOnlyDictionary<string, object?> fields)
        {
            var req = new Dictionary<string, object?>(fields.Count + 1) { ["cmd"] = cmd };
            foreach (var kv in fields)
            {
                if (kv.Value is null) continue; // "save":null и т.п. в запрос не попадает
                req[kv.Key] = kv.Value;
            }
            return SendRaw(JsonSerializer.Serialize(req, JsonOpts));
        }

        /// <summary>Строка трактуется как готовый JSON; остальные значения уходят как есть.</summary>
        private static object AsJson(object value)
        {
            if (value is string raw)
            {
                try
                {
                    using var tmp = JsonDocument.Parse(raw);
                    return tmp.RootElement.Clone();
                }
                catch (JsonException ex)
                {
                    throw new AgentForgeException(
                        "Строка должна быть корректным JSON, например {\"type\":\"Node\"}.",
                        null, null, ex);
                }
            }
            return value;
        }

        /// <summary>Собирает узел-меш (type/name/mesh/props) и добавляет его через node.add.</summary>
        private string AddMeshNode(string parent, string name, string type,
            Dictionary<string, object?> mesh, double[]? position, double[]? color,
            bool staticBody = false, bool body = false,
            string? meshClass = null, Dictionary<string, object?>? extraProps = null)
        {
            var props = new Dictionary<string, object?>();
            if (position != null) props["position"] = position;
            if (color != null) props["color"] = color;
            if (staticBody) props["static_body"] = true;
            if (body) props["body"] = true;
            if (meshClass != null) props["mesh_class"] = meshClass;
            if (extraProps != null)
                foreach (var kv in extraProps) props[kv.Key] = kv.Value;

            var node = new Dictionary<string, object?>
            {
                ["type"] = type,
                ["name"] = name,
                ["mesh"] = mesh,
            };
            if (props.Count > 0) node["props"] = props;
            return NodeAdd(parent, node);
        }

        /// <summary>Возвращает свойство объекта или null (если приёмник не объект / свойства нет).</summary>
        private static JsonElement? GetProp(JsonElement el, string name)
        {
            if (el.ValueKind != JsonValueKind.Object) return null;
            return el.TryGetProperty(name, out JsonElement v) ? v : (JsonElement?)null;
        }

        /// <summary>Возвращает клон свойства ответа; если свойства нет — клон корня.</summary>
        private static JsonElement CloneProp(JsonDocument doc, string prop)
        {
            var v = GetProp(doc.RootElement, prop);
            return v ?? doc.RootElement.Clone();
        }

        private static string? GetString(JsonElement el, string name)
        {
            var v = GetProp(el, name);
            return v.HasValue && v.Value.ValueKind == JsonValueKind.String
                ? v.Value.GetString()
                : null;
        }

        private static long? GetInt64(JsonElement el, string name)
        {
            var v = GetProp(el, name);
            return v.HasValue && v.Value.ValueKind == JsonValueKind.Number &&
                   v.Value.TryGetInt64(out long l)
                ? l
                : (long?)null;
        }

        private static double? GetDouble(JsonElement el, string name)
        {
            var v = GetProp(el, name);
            return v.HasValue && v.Value.ValueKind == JsonValueKind.Number &&
                   v.Value.TryGetDouble(out double d)
                ? d
                : (double?)null;
        }

        private static bool GetBool(JsonElement el, string name, bool fallback = false)
        {
            var v = GetProp(el, name);
            if (!v.HasValue) return fallback;
            if (v.Value.ValueKind == JsonValueKind.True) return true;
            if (v.Value.ValueKind == JsonValueKind.False) return false;
            return fallback;
        }

        /// <summary>Читает JSON-массив чисел в double[] (null, если это не массив).</summary>
        private static double[]? ParseDoubleArray(JsonElement? el)
        {
            if (el is JsonElement e && e.ValueKind == JsonValueKind.Array)
            {
                var list = new List<double>(e.GetArrayLength());
                foreach (var v in e.EnumerateArray())
                    if (v.ValueKind == JsonValueKind.Number) list.Add(v.GetDouble());
                return list.ToArray();
            }
            return null;
        }

        /// <summary>Читает поле "scores" (объект строка→число) в словарь.</summary>
        private static Dictionary<string, double> ReadScores(JsonElement root)
        {
            var scores = new Dictionary<string, double>();
            if (GetProp(root, "scores") is JsonElement sc && sc.ValueKind == JsonValueKind.Object)
            {
                foreach (var p in sc.EnumerateObject())
                    if (p.Value.ValueKind == JsonValueKind.Number)
                        scores[p.Name] = p.Value.GetDouble();
            }
            return scores;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(AgentBridge));
        }
    }
}
