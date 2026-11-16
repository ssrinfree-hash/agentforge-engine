# Blender → AgentForge мост: экспорт .blend-сцены в формат движка AgentForge.
#
# Запуск (Blender 3.x / 4.x, headless):
#   blender --background scene.blend --python blender_bridge.py -- --out /path/out_dir
#
# Результат в out_dir:
#   scene.afscene          — сцена движка (JSON, format "agentforge-scene", version 1)
#   assets/<Имя>.obj(+mtl) — по одному OBJ на каждый меш-объект Blender
#
# Что экспортируется:
#   MESH   → MeshInstance3D (mesh.file "assets/<Имя>.obj", position/rotation/scale
#            из объекта, color из Principled Base Color → mat.diffuse_color →
#            fallback [0.8,0.8,0.8], mesh_class = имя коллекции, static_body=true)
#   CAMERA → Camera3D (fov_deg = degrees(2*atan(18/lens)), active у первой камеры)
#   LIGHT (SUN) → Light3D (направление = rotation, intensity = energy/1000 с clamp)
#   EMPTY / прочие типы с детьми → Node3D (держатель иерархии)
#   Анимация (location / rotation_euler / rotation_quaternion / scale keyframes)
#   → нода Animator ребёнком scene root, целевой объект становится её ребёнком
#     (target_path ищется среди детей Animator, как в движке).
#
# GUI не используется; каждая ошибка объекта ловится отдельно и попадает в
# warnings; в конце печатается JSON-сводка в stdout.

import json
import math
import os
import re
import sys

try:
    import bpy
    from mathutils import Euler, Matrix, Quaternion
except ImportError:
    sys.stderr.write("blender_bridge: запускается только внутри Blender "
                     "(blender --background ... --python blender_bridge.py -- ...)\n")
    sys.exit(1)

# ── Утилиты ──────────────────────────────────────────────────────────────────

WARNINGS = []


def warn(msg):
    WARNINGS.append(str(msg))
    print("[blender_bridge] WARNING: %s" % msg)


def r6(v):
    """float с разумной точностью (6 знаков) — чтобы JSON был компактным."""
    return round(float(v), 6)


def sanitize(name, default="node"):
    """Имя, безопасное для путей движка ('/' — разделитель путей)."""
    s = re.sub(r"[^A-Za-z0-9_.\-]", "_", str(name or "")).strip("_")
    return s if s else default


def unique_name(base, used):
    """Уникальное имя среди детей одного родителя (движок требует уникальности)."""
    name, i = base, 1
    while name in used:
        i += 1
        name = "%s_%d" % (base, i)
    used.add(name)
    return name


def parse_cli_args():
    """Аргументы после '--' в командной строке Blender."""
    argv = sys.argv
    rest = argv[argv.index("--") + 1:] if "--" in argv else []
    out_dir, scene_name = None, "scene"
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--out" and i + 1 < len(rest):
            out_dir = rest[i + 1]
            i += 2
        elif a == "--scene-name" and i + 1 < len(rest):
            scene_name = sanitize(rest[i + 1], "scene")
            i += 2
        elif a in ("-h", "--help"):
            print("usage: blender --background scene.blend --python blender_bridge.py "
                  "-- --out DIR [--scene-name NAME]")
            i += 1
        else:
            warn("unknown argument: %s" % a)
            i += 1
    if not out_dir:
        sys.stderr.write("blender_bridge: не задан --out DIR\n")
        sys.exit(2)
    return out_dir, scene_name


# ── Материалы → цвет ─────────────────────────────────────────────────────────

def material_rgb(obj):
    """Цвет меша: Principled BSDF Base Color → mat.diffuse_color → [0.8,0.8,0.8]."""
    fallback = [0.8, 0.8, 0.8]
    try:
        for slot in obj.material_slots or []:
            mat = slot.material
            if mat is None:
                continue
            if getattr(mat, "use_nodes", False) and mat.node_tree:
                for n in mat.node_tree.nodes:
                    if n.type == "BSDF_PRINCIPLED":
                        inp = n.inputs.get("Base Color")
                        if inp is not None and not inp.is_linked and \
                                hasattr(inp, "default_value"):
                            c = inp.default_value
                            return [r6(c[0]), r6(c[1]), r6(c[2])]
            # viewport-цвет материала (у 4.x это diffuse_color, у части версий
            # есть mat.viewport_display.color) — оба варианта через getattr
            vd = getattr(mat, "viewport_display", None)
            c = getattr(vd, "color", None) if vd is not None else None
            if c is None:
                c = getattr(mat, "diffuse_color", None)
            if c is not None and len(c) >= 3:
                return [r6(c[0]), r6(c[1]), r6(c[2])]
    except Exception as e:  # noqa: BLE001 — материал не должен ронять экспорт
        warn("material color for %r: %s" % (getattr(obj, "name", "?"), e))
    return fallback


# ── Трансформы ───────────────────────────────────────────────────────────────

def transform_props(mat):
    """position / rotation_euler_deg / rotation_quat_xyzw / scale из матрицы.

    Даём оба представления поворота: rotation_quat_xyzw — точный (движок
    применит его последним), rotation_euler_deg — человекочитаемый (XYZ).
    """
    loc, quat, scl = mat.decompose()
    eul = quat.to_euler("XYZ")
    return {
        "position": [r6(loc.x), r6(loc.y), r6(loc.z)],
        "rotation_euler_deg": [r6(math.degrees(eul.x)), r6(math.degrees(eul.y)),
                               r6(math.degrees(eul.z))],
        "rotation_quat_xyzw": [r6(quat.x), r6(quat.y), r6(quat.z), r6(quat.w)],
        "scale": [r6(scl.x), r6(scl.y), r6(scl.z)],
    }


# ── Анимация ─────────────────────────────────────────────────────────────────

def collect_fcurves(action):
    """FCurve списка экшена; поддерживает и slotted actions (Blender 4.4+)."""
    try:
        fcs = list(action.fcurves)
        if fcs:
            return fcs
    except Exception:  # noqa: BLE001
        pass
    try:
        fcs = []
        for layer in action.layers:
            for strip in layer.strips:
                for bag in getattr(strip, "channelbags", []):
                    fcs.extend(bag.fcurves)
        return fcs
    except Exception as e:  # noqa: BLE001
        warn("cannot read fcurves of action %r: %s" % (getattr(action, "name", "?"), e))
        return []


def extract_keyframes(obj, fps):
    """Keyframes [{t,pos,rot_quat_xyzw,scale}] по keyframe_points треков
    location/rotation_euler|rotation_quaternion/scale. t = frame / fps."""
    ad = getattr(obj, "animation_data", None)
    action = getattr(ad, "action", None) if ad else None
    if action is None or fps <= 0:
        return None, None
    fcs = collect_fcurves(action)
    groups = {}  # data_path → {index: fcurve}
    for fc in fcs:
        try:
            dp = fc.data_path
            if dp in ("location", "rotation_euler", "rotation_quaternion", "scale"):
                groups.setdefault(dp, {})[fc.array_index] = fc
        except Exception:  # noqa: BLE001
            continue
    rot_path = "rotation_quaternion" if obj.rotation_mode == "QUATERNION" \
        else "rotation_euler"
    if obj.rotation_mode == "AXIS_ANGLE":
        warn("%r: rotation_mode AXIS_ANGLE не поддерживается — берём статичный поворот"
             % obj.name)
        rot_path = "rotation_euler"
    if "location" not in groups and "scale" not in groups and rot_path not in groups:
        return None, None

    frames = set()
    for g in groups.values():
        for fc in g.values():
            for kp in fc.keyframe_points:
                frames.add(round(float(kp.co.x), 4))
    if len(frames) < 2:
        warn("%r: менее 2 keyframes — анимация пропущена" % obj.name)
        return None, None

    cur_eul = obj.rotation_euler if obj.rotation_mode != "QUATERNION" else None
    cur_quat = obj.rotation_quaternion
    keyframes = []
    for f in sorted(frames):
        t = r6(f / fps)
        # location
        loc = []
        g = groups.get("location", {})
        for i in range(3):
            loc.append(r6(g[i].evaluate(f)) if i in g else r6(obj.location[i]))
        # scale
        scl = []
        g = groups.get("scale", {})
        for i in range(3):
            scl.append(r6(g[i].evaluate(f)) if i in g else r6(obj.scale[i]))
        # rotation → кватернион (xyzw, как ждёт движок)
        if rot_path == "rotation_quaternion":
            g = groups.get("rotation_quaternion", {})
            # Blender-каналы: 0=W, 1=X, 2=Y, 3=Z
            w = r6(g[0].evaluate(f)) if 0 in g else r6(cur_quat.w)
            x = r6(g[1].evaluate(f)) if 1 in g else r6(cur_quat.x)
            y = r6(g[2].evaluate(f)) if 2 in g else r6(cur_quat.y)
            z = r6(g[3].evaluate(f)) if 3 in g else r6(cur_quat.z)
            quat = Quaternion((w, x, y, z))
        else:
            g = groups.get("rotation_euler", {})
            ex = r6(g[0].evaluate(f)) if 0 in g else (r6(cur_eul.x) if cur_eul else 0.0)
            ey = r6(g[1].evaluate(f)) if 1 in g else (r6(cur_eul.y) if cur_eul else 0.0)
            ez = r6(g[2].evaluate(f)) if 2 in g else (r6(cur_eul.z) if cur_eul else 0.0)
            quat = Euler((ex, ey, ez), "XYZ").to_quaternion()
        keyframes.append({
            "t": t,
            "pos": loc,
            "rot_quat_xyzw": [r6(quat.x), r6(quat.y), r6(quat.z), r6(quat.w)],
            "scale": scl,
        })
    anim_class = sanitize(getattr(action, "name", ""), "import")
    return keyframes, anim_class


# ── Экспорт OBJ ──────────────────────────────────────────────────────────────

def export_object_obj(obj, filepath):
    """Экспорт одного объекта в OBJ. Геометрия пишется в ЛОКАЛЬНЫХ координатах:
    на время экспорта matrix_world объекта обнуляется и восстанавливается,
    поэтому трансформ места живёт в ноде .afscene, а не в вершинах файла.
    Возвращает имя использованного оператора."""
    saved = obj.matrix_world.copy()
    obj.matrix_world = Matrix.Identity(4)
    try:
        bpy.context.view_layer.update()
        try:
            # Blender 3.6+ / 4.x — новый C++ экспортёр
            bpy.ops.wm.obj_export(filepath=filepath, export_selected_objects=True)
            return "wm.obj_export"
        except Exception:  # noqa: BLE001 — старый Python-экспортёр (Blender ≤3.x)
            try:
                bpy.ops.export_scene.obj(
                    filepath=filepath, use_selection=True, use_mesh_modifiers=True)
                return "export_scene.obj"
            except Exception:  # noqa: BLE001 — минимальный набор аргументов
                bpy.ops.export_scene.obj(filepath=filepath, use_selection=True)
                return "export_scene.obj(minimal)"
    finally:
        obj.matrix_world = saved
        try:
            bpy.context.view_layer.update()
        except Exception:  # noqa: BLE001
            pass


def select_only(obj):
    """Выделить один объект без GUI-операторов (работает в --background)."""
    for o in bpy.context.view_layer.objects:
        try:
            o.select_set(False)
        except Exception:  # noqa: BLE001
            pass
    obj.select_set(True)
    try:
        bpy.context.view_layer.objects.active = obj
    except Exception:  # noqa: BLE001
        pass


def export_mesh_obj(obj, assets_dir, obj_files):
    """Экспорт меша в assets/<Имя>.obj и регистрация файла. Восстановление
    matrix_world происходит в finally внутри export_object_obj, поэтому
    дальнейшие чтения трансформов объекта корректны."""
    safe = sanitize(obj.name, "mesh")
    filepath = os.path.join(assets_dir, safe + ".obj")
    select_only(obj)
    used_op = export_object_obj(obj, filepath)
    rel = "assets/%s.obj" % safe
    obj_files.append(rel)
    print("[blender_bridge] obj: %s (%s)" % (rel, used_op))
    return rel


# ── Основной проход ──────────────────────────────────────────────────────────

def build_scene(scene, out_dir, assets_dir):
    """Собирает {'scene': dict, 'stats': {...}, 'obj_files': [...]}."""
    view_layer_objects = list(scene.view_layers[0].objects) \
        if getattr(scene, "view_layers", None) else list(bpy.context.view_layer.objects)
    # скрываемые из рендера объекты пропускаем
    objects = [o for o in view_layer_objects if not getattr(o, "hide_render", False)]
    skipped_hidden = [o.name for o in view_layer_objects if getattr(o, "hide_render", False)]
    if skipped_hidden:
        warn("skip hide_render objects: %s" % ", ".join(skipped_hidden))

    # fps для пересчёта frame → секунды
    fps = float(scene.render.fps or 0)
    fps_base = float(getattr(scene.render, "fps_base", 1.0) or 1.0)
    fps_eff = fps / fps_base if fps > 0 else 24.0
    if fps <= 0:
        warn("scene.render.fps = 0 — анимация считается с fps=24")

    # порядок «родители раньше детей» (по глубине цепочки parent)
    def depth(o):
        d, p = 0, o.parent
        while p is not None and p in objects:
            d += 1
            p = p.parent
        return d

    objects.sort(key=depth)

    root_children = []
    node_of = {}          # Blender object → AF-нода
    name_used_top = set()  # имена среди детей root
    cam_nodes = []        # все Camera3D-ноды (первая станет active)
    stats = {"meshes": 0, "cameras": 0, "lights": 0, "animators": 0, "others": 0}
    obj_files = []

    for obj in objects:
        try:
            # анимация объекта (если есть)
            keyframes, anim_class = extract_keyframes(obj, fps_eff)

            # узел для объекта
            node = {"type": "Node", "name": obj.name, "props": {}}
            props = node["props"]
            is_mesh = obj.type == "MESH"
            is_cam = obj.type == "CAMERA"
            is_sun = obj.type == "LIGHT" and getattr(obj.data, "type", "") == "SUN"

            if is_mesh:
                node["type"] = "MeshInstance3D"
                safe = sanitize(obj.name, "mesh")
                rel = "assets/%s.obj" % safe
                props["mesh"] = {"file": rel}
                props["color"] = material_rgb(obj)
                props["visible"] = True
                props["static_body"] = True
                colls = [c.name for c in (obj.users_collection or []) if c]
                props["mesh_class"] = sanitize(colls[0], sanitize(obj.name, "mesh")) \
                    if colls else sanitize(obj.name, "mesh")
                export_mesh_obj(obj, assets_dir, obj_files)
                stats["meshes"] += 1
            elif is_cam:
                node["type"] = "Camera3D"
                cam = obj.data
                lens = float(cam.lens or 50.0)
                props["fov_deg"] = r6(math.degrees(2.0 * math.atan(18.0 / lens)))
                props["near_z"] = r6(getattr(cam, "clip_start", 0.05))
                props["far_z"] = r6(getattr(cam, "clip_end", 200.0))
                cam_nodes.append(node)
                stats["cameras"] += 1
            elif is_sun:
                node["type"] = "Light3D"
                light = obj.data
                props["color"] = [r6(light.color[0]), r6(light.color[1]),
                                  r6(light.color[2])]
                props["intensity"] = r6(min(max(float(light.energy) / 1000.0, 0.0), 10.0))
                stats["lights"] += 1
            elif obj.type in ("EMPTY",) or (obj.children):
                node["type"] = "Node3D"      # держатель иерархии
                stats["others"] += 1
            else:
                if obj.type == "LIGHT":
                    warn("light %r: тип %r не поддерживается (нужен SUN) — пропущен"
                         % (obj.name, getattr(obj.data, "type", "?")))
                else:
                    warn("object %r: тип %r не экспортируется — пропущен"
                         % (obj.name, obj.type))
                continue

            node["name"] = sanitize(obj.name, node["type"])
            if keyframes:
                # Animator — ребёнок scene root; объект — ребёнок Animator
                # (target_path ищется среди детей Animator, как в движке).
                anim_name = unique_name("Anim_" + node["name"], name_used_top)
                node["name"] = unique_name(node["name"], name_used_top)
                # мировой трансформ запекается в ноду объекта (Animator — identity)
                props.update(transform_props(obj.matrix_world))
                animator = {
                    "type": "Animator",
                    "name": anim_name,
                    "props": {
                        "target_path": node["name"],
                        "playing": True,
                        "loop": True,
                        "anim_class": anim_class or "import",
                        "keyframes": keyframes,
                    },
                    "children": [node],
                }
                root_children.append(animator)
                stats["animators"] += 1
                node_of[obj] = node
                continue  # дети прикрепятся к node ниже

            # статичный трансформ: локальный относительно родителя (если он экспортирован)
            if obj.parent is not None and obj.parent in node_of:
                parent_world = obj.parent.matrix_world
                local = parent_world.inverted() @ obj.matrix_world
                props.update(transform_props(local))
            else:
                props.update(transform_props(obj.matrix_world))

            # прикрепление к родителю или к root
            if obj.parent is not None and obj.parent in node_of:
                pnode = node_of[obj.parent]
                pnode.setdefault("children", []).append(node)
            else:
                node["name"] = unique_name(node["name"], name_used_top)
                root_children.append(node)
            node_of[obj] = node
        except Exception as e:  # noqa: BLE001 — ошибка одного объекта не роняет всё
            warn("object %r: %s: %s" % (getattr(obj, "name", "?"), type(e).__name__, e))

    # первая камера (порядок обхода) — активная, остальные нет
    if cam_nodes:
        for i, c in enumerate(cam_nodes):
            c["props"]["active"] = (i == 0)
    else:
        warn("в сцене нет камеры — frame.capture вернёт ошибку "
             "'no active Camera3D in scene'")

    scene_json = {
        "format": "agentforge-scene",
        "version": 1,
        "time": 0,
        "scores": {},
        "root": {"type": "SceneTree", "name": "root", "children": root_children},
    }
    return scene_json, stats, obj_files


# ── Точка входа ──────────────────────────────────────────────────────────────

def main():
    out_dir, scene_name = parse_cli_args()
    try:
        os.makedirs(out_dir, exist_ok=True)
        assets_dir = os.path.join(out_dir, "assets")
        os.makedirs(assets_dir, exist_ok=True)
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("blender_bridge: cannot create %s: %s\n" % (out_dir, e))
        sys.exit(2)

    scene = bpy.context.scene
    # объектный режим (на случай сохранённой сцены в edit mode)
    try:
        if bpy.context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
    except Exception:  # noqa: BLE001
        pass

    stats = {"meshes": 0, "cameras": 0, "lights": 0, "animators": 0, "others": 0}
    obj_files = []
    scene_json = None
    try:
        scene_json, stats, obj_files = build_scene(scene, out_dir, assets_dir)
    except Exception as e:  # noqa: BLE001
        warn("build_scene failed: %s: %s" % (type(e).__name__, e))

    scene_path = os.path.join(out_dir, scene_name + ".afscene")
    if scene_json is not None:
        try:
            with open(scene_path, "w", encoding="utf-8") as f:
                json.dump(scene_json, f, ensure_ascii=False, indent=2)
        except Exception as e:  # noqa: BLE001
            warn("cannot write %s: %s" % (scene_path, e))

    summary = {
        "blender_bridge": {
            "blender": ".".join(str(v) for v in bpy.app.version[:3]),
            "source_scene": bpy.data.filepath or "(unsaved)",
            "out_dir": out_dir,
            "scene_file": scene_path if scene_json is not None else None,
            "exported": {
                "obj_files": len(obj_files),
                "mesh_nodes": stats["meshes"],
                "cameras": stats["cameras"],
                "lights": stats["lights"],
                "animators": stats["animators"],
                "other_nodes": stats["others"],
            },
            "warnings": WARNINGS,
        }
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))


main()
