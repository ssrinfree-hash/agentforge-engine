// ---------------------------------------------------------------------------
// OrbRun.cs — пример небольшой «игры» на AgentForge SDK.
//
// Сцена:
//   - земля-плоскость 20 x 20 (primitive "plane", static_body);
//   - 10 монет: сферы radius 0.3 с mesh_class "coin" по кругу радиуса 5;
//   - игрок-капсула в центре арены (props.body = true);
//   - камера Camera3D сверху под 45° и источник света Light3D.
//
// Игровой цикл (600 кадров): игрок движется к ближайшей монете
// (node.set position/velocity); когда горизонтальная дистанция до монеты
// становится меньше 0.6, монета удаляется (node.remove), счёт увеличивается
// на 1 и отправляется движку (scores.set). В конце — frame.capture и сводка.
//
// Запуск:  dotnet run --project AgentForge.csproj -- afengine
// (после `--` передаётся путь к бинарнику движка; по умолчанию "afengine").
// ---------------------------------------------------------------------------
using System;
using System.Collections.Generic;
using AgentForgeSdk;
using System.Text.Json;

namespace AgentForgeSdk.Examples
{
    /// <summary>Пример: игрок-капсула собирает монеты на круглой арене.</summary>
    public static class OrbRun
    {
        private const string DefaultEngine = "afengine";
        private const int CoinCount = 10;        // сколько монет расставить
        private const double RingRadius = 5.0;   // радиус круга с монетами
        private const double CoinRadius = 0.3;   // радиус монеты-сферы
        private const double PickupDist = 0.6;   // дистанция сбора монеты
        private const double Speed = 3.0;        // скорость игрока, ед./с
        private const double Dt = 1.0 / 60.0;    // шаг симуляции, с
        private const int TotalFrames = 600;     // длительность игры в кадрах

        /// <summary>Точка входа. args[0] — путь к бинарнику движка.</summary>
        /// <returns>0 — успех, 1 — ошибка.</returns>
        public static int Main(string[] args)
        {
            string enginePath = args.Length > 0 ? args[0] : DefaultEngine;
            Console.WriteLine($"[OrbRun] Запуск движка: '{enginePath}'");

            try
            {
                using var engine = new AgentBridge(enginePath);
                Play(engine);
                return 0;
            }
            catch (AgentForgeException ex)
            {
                Console.Error.WriteLine($"[OrbRun] Ошибка протокола: {ex.Message}");
                if (ex.Error != null)
                    Console.Error.WriteLine($"[OrbRun] Поле error движка: {ex.Error}");
                return 1;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[OrbRun] Сбой: {ex.Message}");
                return 1;
            }
        }

        /// <summary>Строит арену и проигрывает сбор монет.</summary>
        private static void Play(AgentBridge engine)
        {
            // --- Проверка связи -------------------------------------------------
            var ping = engine.Ping();
            string engineName = GetString(ping, "engine") ?? "?";
            string engineVersion = GetString(ping, "version") ?? "?";
            Console.WriteLine($"[OrbRun] Движок: {engineName} v{engineVersion}");

            // --- Демонстрация поиска ассетов ------------------------------------
            var found = engine.AssetSearch("coin", 3);
            string foundModel = found.Model ?? "?";
            Console.WriteLine(
                $"[OrbRun] asset.search \"coin\": модель={foundModel}, " +
                $"в каталоге {found.NCatalog} ассетов, найдено {found.Results.Count}");

            // --- Новая сцена и арена --------------------------------------------
            engine.SceneNew();

            // Земля: плоскость 20 x 20 со статичным телом.
            engine.AddPlane("root", "Ground", new[] { 20.0, 1.0, 20.0 },
                new[] { 0.0, 0.0, 0.0 }, new[] { 0.30, 0.52, 0.28 });

            // 10 монет-сфер по кругу радиуса 5; координаты запоминаем локально —
            // монеты статичны, поэтому в цикле их спрашивать у движка не нужно.
            var coinPaths = new List<string>();
            var coinPos = new List<double[]>();
            for (int i = 0; i < CoinCount; i++)
            {
                double angle = 2.0 * Math.PI * i / CoinCount;
                var pos = new[]
                {
                    Math.Cos(angle) * RingRadius,
                    CoinRadius,
                    Math.Sin(angle) * RingRadius,
                };
                string path = engine.AddSphere("root", $"Coin{i + 1:00}", CoinRadius,
                    pos, new[] { 1.0, 0.84, 0.10 }, staticBody: true, meshClass: "coin");
                coinPaths.Add(path);
                coinPos.Add(pos);
            }

            // Игрок-капсула с динамическим телом в центре арены.
            string playerPath = engine.AddCapsule("root", "Player", 0.35, 1.2,
                new[] { 0.0, 0.9, 0.0 }, new[] { 0.20, 0.46, 0.92 }, body: true);

            // Камера сверху под 45° и направленный свет.
            engine.AddCamera("root", "MainCamera",
                new[] { 0.0, 12.0, 10.0 }, new[] { -45.0, 0.0, 0.0 });
            engine.AddLight("root", "Sun",
                new[] { -60.0, 30.0, 0.0 }, new[] { 5.0, 10.0, 5.0 });

            Console.WriteLine(
                "[OrbRun] Арена готова: земля 20x20, монет: " +
                $"{coinPaths.Count}, игрок в центре, камера под 45°.");

            // --- Игровой цикл ----------------------------------------------------
            int score = 0;              // собранные монеты = счёт
            int framesStepped = 0;      // всего отработанных кадров симуляции
            long eventsSeen = 0;        // событий, вернувшихся из sim.step
            double simTime = 0.0;       // симуляционное время после последнего шага

            for (int frame = 0; frame < TotalFrames && coinPaths.Count > 0; frame++)
            {
                // Позиция игрока — единственное, что двигается, поэтому читаем её
                // у движка каждый кадр (node.get).
                var player = engine.NodeGet(playerPath);
                var pos = GetVector3(player, "position") ?? new[] { 0.0, 0.0, 0.0 };

                // Ближайшая монета: считаем ГОРИЗОНТАЛЬНУЮ (XZ) дистанцию,
                // т.к. игрок и монета стоят на разной высоте.
                int best = -1;
                double bestDist = double.MaxValue;
                for (int i = 0; i < coinPos.Count; i++)
                {
                    double dx = coinPos[i][0] - pos[0];
                    double dz = coinPos[i][2] - pos[2];
                    double d = Math.Sqrt(dx * dx + dz * dz);
                    if (d < bestDist) { bestDist = d; best = i; }
                }
                if (best < 0) break;

                // Сбор монеты: удалить узел, увеличить счёт, отправить движку.
                if (bestDist < PickupDist)
                {
                    engine.NodeRemove(coinPaths[best]);
                    coinPaths.RemoveAt(best);
                    coinPos.RemoveAt(best);
                    score++;
                    engine.ScoresSet(new Dictionary<string, double> { ["score"] = score });
                    Console.WriteLine(
                        $"[OrbRun] Кадр {frame,4}: монета собрана, " +
                        $"счёт={score}, осталось={coinPaths.Count}.");
                    continue;
                }

                // Движение к монете: задаём и позицию (прогноз на один шаг),
                // и скорость — физика использует velocity между кадрами.
                double k = Speed / bestDist;
                var vel = new[]
                {
                    (coinPos[best][0] - pos[0]) * k,
                    0.0,
                    (coinPos[best][2] - pos[2]) * k,
                };
                engine.NodeSet(playerPath, new Dictionary<string, object?>
                {
                    ["position"] = new[] { pos[0] + vel[0] * Dt, pos[1], pos[2] + vel[2] * Dt },
                    ["velocity"] = vel,
                });

                var step = engine.SimStep(1, Dt);
                framesStepped += step.Frames;
                eventsSeen += step.Events.Length;
                simTime = step.Time;
            }

            if (coinPaths.Count > 0)
                Console.WriteLine("[OrbRun] 600 кадров закончились — не все монеты собраны.");

            // --- Итоговый кадр и сводка ------------------------------------------
            var report = engine.FrameCapture(640, 360);
            Console.WriteLine();
            Console.WriteLine("[OrbRun] ================= ИТОГИ =================");
            Console.WriteLine($"  собрано монет:        {score} из {CoinCount}");
            Console.WriteLine(
                $"  кадров симуляции:     {framesStepped}, " +
                $"сим-время: {simTime:F2} с, событий: {eventsSeen}");
            Console.WriteLine(
                $"  финальный кадр:       #{report.Frame}, время {report.SimTime:F2} с, " +
                $"разрешение {report.Resolution?[0]}x{report.Resolution?[1]}");
            Console.WriteLine(
                $"  рендер:               объектов в кадре {report.Objects.Count}, " +
                $"треугольников {report.DrawnTris}, {report.RenderMs:F2} мс");
            if (report.Scores != null && report.Scores.Count > 0)
            {
                var parts = new List<string>();
                foreach (var p in report.Scores) parts.Add($"{p.Key}={p.Value}");
                Console.WriteLine("  счёт движка:          " + string.Join(", ", parts));
            }
            foreach (var o in report.Objects)
            {
                Console.WriteLine(
                    $"   - {o.Name,-12} {o.Path,-14} " +
                    $"bbox=[{Fmt(o.ScreenBBox)}] tris={o.Tris} visible={o.Visible}");
            }
            Console.WriteLine("[OrbRun] Готово.");

            // Вежливое завершение движка (quit + dispose).
            engine.Quit();
        }

        /// <summary>Форматирует массив чисел как "a,b,c" (или "-", если null).</summary>
        private static string Fmt(double[]? a) => a == null ? "-" : string.Join(",", a);

        /// <summary>Читает строковое свойство JSON-объекта.</summary>
        private static string? GetString(JsonElement el, string name)
        {
            if (el.ValueKind == JsonValueKind.Object &&
                el.TryGetProperty(name, out JsonElement v) &&
                v.ValueKind == JsonValueKind.String)
                return v.GetString();
            return null;
        }

        /// <summary>Читает числовой массив (позицию) из JSON-объекта узла.</summary>
        private static double[]? GetVector3(JsonElement el, string name)
        {
            if (el.ValueKind == JsonValueKind.Object &&
                el.TryGetProperty(name, out JsonElement v) &&
                v.ValueKind == JsonValueKind.Array)
            {
                var list = new List<double>();
                foreach (var x in v.EnumerateArray())
                    if (x.ValueKind == JsonValueKind.Number) list.Add(x.GetDouble());
                if (list.Count >= 2) return list.ToArray();
            }
            return null;
        }
    }
}
