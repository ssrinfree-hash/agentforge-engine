#!/usr/bin/env python
"""OrbRun — многофазная сборка и прогон тестовой игры через агент-протокол.

Фазы (по спеке Agnes илиbrun_spec.json):
  1 design  — чтение спеки
  2 assets  — примитивы движка + демонстрация asset.search
  3 scene   — сборка арены через JSON-RPC (ground, стены, монеты+аниматоры, игрок, камера, свет)
  4 logic   — plugin.load игрового плагина
  5 test    — playthrough headless: sim.step + события + кадры
  6 report  — game_report.json (+ отдельный Agnes-QA вызывается снаружи)

Запуск: python orbrun_build.py [path_to_afengine]
Выход: /home/z/my-project/agentforge/game/game_report.json, frames в game/frames/
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent          # agentforge/
ENGINE = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "build" / "afengine"
PLUGIN = HERE / "build" / "game_logic.so"
SPEC = HERE / "game" / "orbrun_spec.json"
GAME_DIR = HERE / "game"
FRAMES = GAME_DIR / "frames"


def light_quat(spec):
    """Кватерпион: направить свет (0,0,-1) вниз на арену (dir 0.35,-0.85,-0.4)."""
    import math
    target = (0.35, -0.85, -0.4)
    v = (0.0, 0.0, -1.0)
    t = target
    axis = (v[1] * t[2] - v[2] * t[1], v[2] * t[0] - v[0] * t[2], v[0] * t[1] - v[1] * t[0])
    w = 1.0 + v[0] * t[0] + v[1] * t[1] + v[2] * t[2]
    n = math.sqrt(axis[0] ** 2 + axis[1] ** 2 + axis[2] ** 2 + w * w) or 1.0
    return [axis[0] / n, axis[1] / n, axis[2] / n, w / n]


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([str(path), "serve"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.n = 0

    def cmd(self, **req):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("engine died")
        return json.loads(line)

    def close(self):
        try:
            self.cmd(cmd="quit")
        except Exception:
            pass
        self.p.wait(timeout=10)


def main() -> None:
    t0 = time.time()
    report = {"game": "OrbRun", "phases": []}

    def phase(name, goal):
        ph = {"name": name, "goal": goal, "started": time.time() - t0}
        report["phases"].append(ph)
        print(f"=== Фаза {len(report['phases'])}: {name} — {goal}")
        return ph

    eng = Engine(ENGINE)
    spec = json.loads(SPEC.read_text())
    coins_spec, player_spec = spec["coins"], spec["player"]

    # ── 1 design ──
    ph = phase("design", "спека от Agnes 3.0-flash")
    ph["spec"] = spec

    # ── 2 assets ──
    ph = phase("assets", "примитивы движка + проверка asset.search")
    r = eng.cmd(cmd="asset.search", query="chair", top_k=3)
    ph["asset_search_model"] = r["search"].get("model")
    ph["primitives"] = ["plane", "box", "sphere", "capsule"]

    # ── 3 scene ──
    ph = phase("scene", "арена через JSON-RPC")
    eng.cmd(cmd="scene.new")
    gs = spec["arena"]["ground_size"]
    eng.cmd(cmd="node.add", parent="root", node={
        "type": "MeshInstance3D", "name": "Ground",
        "mesh": {"primitive": "plane", "size": [gs]},
        "props": {"static_body": True, "color": spec["arena"]["ground_color"]}})
    hw = gs / 2
    for i, (px, pz, rot) in enumerate([(-hw, 0, 90), (hw, 0, 90), (0, -hw, 0), (0, hw, 0)]):
        eng.cmd(cmd="node.add", parent="root", node={
            "type": "MeshInstance3D", "name": f"Wall{i}",
            "mesh": {"primitive": "box", "size": [gs, 2, 1]},
            "props": {"position": [px, 1, pz], "rotation_euler_deg": [0, rot, 0],
                      "static_body": True, "color": spec["arena"]["wall_color"]}})
    coin_paths, anim_paths = [], []
    import math
    n = coins_spec["count"]
    for i in range(n):
        ang = 2 * math.pi * i / n
        cx, cz = coins_spec["ring_radius"] * math.cos(ang), coins_spec["ring_radius"] * math.sin(ang)
        r1 = eng.cmd(cmd="node.add", parent="root", node={
            "type": "Animator", "name": f"Spin{i}", "anim_class": "spin"})
        spin = f"Spin{i}"
        anim_paths.append(spin)
        r2 = eng.cmd(cmd="node.add", parent=spin, node={
            "type": "MeshInstance3D", "name": f"Coin{i}",
            "mesh": {"primitive": "sphere", "radius": coins_spec["radius"]},
            "props": {"position": [cx, 1.0, cz],
                      "color": coins_spec["color"], "mesh_class": "coin"}})
        coin_paths.append(f"{spin}/Coin{i}")
        eng.cmd(cmd="node.set", path=spin, props={"keyframes": [
            {"t": 0, "pos": [cx, 1.0, cz], "rot_quat_xyzw": [0, 0, 0, 1], "scale": [1, 1, 1]},
            {"t": coins_spec["spin_seconds"] / 2, "pos": [cx, 1.6, cz],
             "rot_quat_xyzw": [0, 0.7071, 0, 0.7071], "scale": [1, 1, 1]},
            {"t": coins_spec["spin_seconds"], "pos": [cx, 1.0, cz],
             "rot_quat_xyzw": [0, 0.9239, 0, 0.3827], "scale": [1, 1, 1]}], "loop": True})
    # препятствия
    obs = spec.get("obstacles", {})
    for i in range(obs.get("count", 0)):
        ang = 2 * math.pi * (i + 0.5) / max(obs["count"], 1)
        ox, oz = coins_spec["ring_radius"] * 0.45 * math.cos(ang), coins_spec["ring_radius"] * 0.45 * math.sin(ang)
        eng.cmd(cmd="node.add", parent="root", node={
            "type": "MeshInstance3D", "name": f"Obs{i}",
            "mesh": {"primitive": "box", "size": obs["size"]},
            "props": {"position": [ox, obs["size"][1] / 2, oz],
                      "rotation_euler_deg": [0, i * 33 % 90, 0],
                      "static_body": True, "color": spec["arena"]["wall_color"],
                      "mesh_class": "obstacle"}})
    eng.cmd(cmd="node.add", parent="root", node={
        "type": "MeshInstance3D", "name": player_spec and "Player",
        "mesh": {"primitive": "capsule", "radius": player_spec["radius"],
                 "height": player_spec["height"]},
        "props": {"position": [0, player_spec["height"] / 2 + 0.05, 0],
                  "color": player_spec["color"], "body": True, "mesh_class": "player"}})
    eng.cmd(cmd="node.add", parent="root", node={
        "type": "Camera3D", "name": "Cam",
        "props": {"position": spec["camera"]["position"],
                  "rotation_euler_deg": spec["camera"]["look_deg"],
                  "fov_deg": spec["camera"]["fov_deg"]}})
    eng.cmd(cmd="node.add", parent="root", node={
        "type": "Light3D", "name": "Sun",
        "props": {"rotation_quat_xyzw": light_quat(spec),
                  "intensity": max(spec["light"]["intensity"], 1.8) * 3.0}})
    save = eng.cmd(cmd="scene.save", path=str(GAME_DIR / "orbrun.afscene"))
    ph["saved"] = save.get("ok", False)
    ph["nodes"] = len(eng.cmd(cmd="scene.tree")["scene"]["children"][0].get("children", []))

    # ── 4 logic ──
    ph = phase("logic", "C++ плагин игровой логики (C ABI)")
    pl = eng.cmd(cmd="plugin.load", path=str(PLUGIN))
    ph["loaded"] = pl.get("ok", False)
    st = eng.cmd(cmd="plugin.call", event={"op": "setup", "player": "Player",
                                           "coins": coin_paths,
                                           "speed": player_spec["speed"],
                                           "pickup_dist": spec["rules"]["pickup_dist"],
                                           "max_seconds": spec["rules"]["max_seconds"]})
    ph["setup"] = st.get("plugin", st)

    # ── 5 test ──
    ph = phase("test", "headless playthrough (sim.step 1с-чанками)")
    FRAMES.mkdir(parents=True, exist_ok=True)
    events_all, frames_saved = [], []
    max_s = spec["rules"]["max_seconds"]
    for chunk in range(1, int(max_s) + 1):
        r = eng.cmd(cmd="sim.step", frames=60, dt=1 / 60.0)
        events_all.extend(r.get("events", []))
        events_all.extend(r.get("plugin_log", []))
        if chunk % 2 == 0 or chunk == 1:
            fr = eng.cmd(cmd="frame.capture", width=640, height=360,
                         save=str(FRAMES / f"frame_{chunk:03d}s.ppm"))
            if fr.get("ok"):
                frames_saved.append(fr["frame"]["saved_ppm"])
        st = eng.cmd(cmd="plugin.call", event={"op": "status"})["plugin"]
        if st.get("finished"):
            break
    final = eng.cmd(cmd="frame.capture", width=640, height=360,
                    save=str(FRAMES / "final.ppm"))
    st = eng.cmd(cmd="plugin.call", event={"op": "status"})["plugin"]
    ph["status"] = st
    ph["events_count"] = len(events_all)
    ph["pickup_events"] = sum(1 for e in events_all if e.get("event") == "log" and "pickup" in e.get("msg", ""))
    ph["frames"] = frames_saved
    ph["final_frame"] = final.get("frame", {}).get("saved_ppm")

    # ── 6 report ──
    ph = phase("report", "game_report.json")
    report["result"] = {
        "score": st.get("score"),
        "coins_left": st.get("coins_left"),
        "win": st.get("score", 0) >= coins_spec["count"],
        "time": st.get("time"),
        "runtime_s": round(time.time() - t0, 2),
    }
    report_path = GAME_DIR / "game_report.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(json.dumps(report["result"], ensure_ascii=False))
    eng.close()


if __name__ == "__main__":
    main()
