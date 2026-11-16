// AgentForge — сцена: дерево нод в стиле Godot (root → children), но всё
// строится и читается через JSON. Каждая нода self-describing (props()).
#pragma once
#include "af/math.h"
#include "af/json.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace af {

class Node;
using NodePtr = std::shared_ptr<Node>;

// ── Mesh: геометрия (позиции, нормали, uv) + индексы ──
struct Mesh {
  std::vector<Vec3> pos;
  std::vector<Vec3> nrm;
  std::vector<Vec2> uv;
  std::vector<uint32_t> idx;         // тройки
  Vec3 color{0.8f, 0.8f, 0.8f};      // базовый цвет материала (из MTL Kd)
  std::string name;
  AABB bounds;
  void recompute_bounds();
  bool valid() const { return !idx.empty() && !pos.empty(); }
};

// ── Node ──
class Node : public std::enable_shared_from_this<Node> {
public:
  uint32_t id = 0;                    // назначает SceneTree
  std::string name;                   // уникально в пределах родителя
  std::string type = "Node";
  std::weak_ptr<Node> parent;
  std::vector<NodePtr> children;
  bool enabled = true;

  explicit Node(const std::string& t) : type(t) {}
  virtual ~Node() = default;

  virtual Json props() const;                       // полная self-описание
  virtual void set_prop(const std::string& k, const Json& v);
  virtual std::vector<std::string> prop_names() const;
  virtual bool is_3d() const { return false; }

  NodePtr find(const std::string& path);            // "Arena/Player/Camera"
  NodePtr child_by_name(const std::string& n) const;
  void add(const NodePtr& c);
  bool remove(const std::string& name);
  std::string path() const;

  virtual void update(float dt, double time_sec);   // обход дерева снизу вверх не нужен — top-down
  void update_tree(float dt, double time_sec);

protected:
  virtual void fill_props(Json& j) const;
};

// ── Node3D: позиция/поворот/масштаб, иерархия трансформов ──
class Node3D : public Node {
public:
  Vec3 position;
  Quat rotation;
  Vec3 scale{1, 1, 1};

  explicit Node3D(const std::string& t) : Node(t) {}
  bool is_3d() const override { return true; }

  Mat4 local_matrix() const { return Mat4::trs(position, rotation, scale); }
  Mat4 world_matrix() const;                        // с учётом родителей (кэш)
  void invalidate() { dirty = true; }
  AABB world_aabb() const;

  Json props() const override;
  void set_prop(const std::string& k, const Json& v) override;
  std::vector<std::string> prop_names() const override;

protected:
  void fill_props(Json& j) const override;
  mutable bool dirty = true;
  mutable Mat4 cached_world;
};

// ── MeshInstance3D ──
class MeshInstance3D : public Node3D {
public:
  std::shared_ptr<Mesh> mesh;
  Vec3 color{0.8f, 0.8f, 0.8f};
  bool visible = true;
  bool static_body = false;          // участвует в физике как статическая коллизия
  bool body = false;                 // динамическое тело (гравитация/коллизии)
  Vec3 velocity;
  std::string mesh_class;            // семантическая метка (для FrameReport и asset.search)

  MeshInstance3D() : Node3D("MeshInstance3D") {}
  Json props() const override;
  void set_prop(const std::string& k, const Json& v) override;
  std::vector<std::string> prop_names() const override;
};

// Мировой AABB меша с учётом трансформа (свободная функция — нужна рендеру и физике)
AABB MeshInstance3D_world_aabb(const MeshInstance3D& mi);

// ── Camera3D ──
class Camera3D : public Node3D {
public:
  float fov_deg = 60.0f;
  float near_z = 0.05f, far_z = 200.0f;
  bool active = true;

  Camera3D() : Node3D("Camera3D") {}
  Mat4 view() const { return world_matrix().inverse(); }
  Mat4 projection(float aspect) const {
    return Mat4::perspective(fov_deg * PI / 180.0f, aspect, near_z, far_z); }
  Json props() const override;
  void set_prop(const std::string& k, const Json& v) override;
  std::vector<std::string> prop_names() const override;
};

// ── Light3D: направленный свет (один активный) ──
class Light3D : public Node3D {
public:
  Vec3 color{1, 1, 1};
  float intensity = 1.0f;
  bool active = true;

  Light3D() : Node3D("Light3D") {}
  Vec3 direction() const { return rotation.rotate(Vec3{0, 0, -1}).normalized(); }
  Json props() const override;
  void set_prop(const std::string& k, const Json& v) override;
  std::vector<std::string> prop_names() const override;
};

// ── Animator: keyframe-треки на трансформ 3D-ноды ──
struct Keyframe { float t; Vec3 pos; Quat rot; Vec3 scl; };
class Animator : public Node3D {
public:
  std::string target_path;           // путь 3D-ноды (может быть сам)
  std::vector<Keyframe> track;
  float length = 0.0f;
  float time = 0.0f;
  bool playing = true;
  bool loop = true;
  std::string anim_class;            // "spin", "bob", ... (для отчётов)

  Animator() : Node3D("Animator") {}
  void update(float dt, double time_sec) override;
  Json props() const override;
  void set_prop(const std::string& k, const Json& v) override;
  std::vector<std::string> prop_names() const override;
};

// ── SceneTree ──
class SceneTree {
public:
  NodePtr root;
  uint32_t next_id = 1;
  double time = 0.0f;
  int frame = 0;
  std::map<std::string, double> scores;   // игровое состояние: score, hp, ... (для агентов)

  SceneTree();
  NodePtr make(const std::string& type);            // фабрика по имени типа
  NodePtr find(const std::string& path) const { return root->find(path); }
  void update(float dt) { time += dt; ++frame; root->update_tree(dt, time); }
  std::vector<std::shared_ptr<MeshInstance3D>> mesh_instances() const;
  std::vector<std::shared_ptr<Camera3D>> cameras() const;
  std::vector<std::shared_ptr<Light3D>> lights() const;
  std::vector<std::shared_ptr<Animator>> animators() const;

  Json to_json() const;                             // вся сцена (для сохранения)
  bool from_json(const Json& j, std::string* err);  // загрузка сцены
  Json tree_json() const;                           // компактное дерево (имена+типы)
};

} // namespace af
