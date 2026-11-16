// AgentForge — реализация сцены: ноды, трансформы, сериализация.
#include "af/scene.h"
#include <cstdio>
#include <set>

namespace af {

// ── Mesh ──
void Mesh::recompute_bounds() {
  bounds = AABB();
  for (auto& p : pos) bounds.grow(p);
}

// ── Node: базовые операции ──
void Node::fill_props(Json& j) const {
  j["name"] = Json::of_str(name);
  j["type"] = Json::of_str(type);
  j["enabled"] = Json::of_bool(enabled);
}
Json Node::props() const { Json j = Json::object(); fill_props(j); return j; }
void Node::set_prop(const std::string& k, const Json& v) {
  if (k == "name") name = v.as_str(name);
  else if (k == "enabled") enabled = v.as_bool(enabled);
}
std::vector<std::string> Node::prop_names() const { return {"name", "type", "enabled"}; }

NodePtr Node::child_by_name(const std::string& n) const {
  for (auto& c : children) if (c->name == n) return c;
  return nullptr;
}
NodePtr Node::find(const std::string& path) {
  size_t slash = path.find('/');
  std::string head = path.substr(0, slash);
  NodePtr c = child_by_name(head);
  if (!c || slash == std::string::npos) return c;
  return c->find(path.substr(slash + 1));
}
void Node::add(const NodePtr& c) {
  c->parent = shared_from_this();
  children.push_back(c);
}
bool Node::remove(const std::string& n) {
  for (size_t i = 0; i < children.size(); ++i)
    if (children[i]->name == n) { children.erase(children.begin() + i); return true; }
  return false;
}
std::string Node::path() const {
  auto p = parent.lock();
  if (!p) return name;
  return p->path() + "/" + name;
}
void Node::update_tree(float dt, double time_sec) {
  for (auto& c : children) c->update_tree(dt, time_sec);
  update(dt, time_sec);
}
void Node::update(float, double) {}

// ── Node3D ──
Mat4 Node3D::world_matrix() const {
  if (!dirty) return cached_world;
  Mat4 local = local_matrix();
  auto p = std::dynamic_pointer_cast<Node3D>(parent.lock());
  cached_world = p ? p->world_matrix() * local : local;
  dirty = false;
  return cached_world;
}
AABB Node3D::world_aabb() const { return AABB(); }
void Node3D::fill_props(Json& j) const {
  Node::fill_props(j);
  Json p = Json::array();
  p.push(Json::of_num(position.x)); p.push(Json::of_num(position.y)); p.push(Json::of_num(position.z));
  j["position"] = p;
  Json r = Json::array();
  r.push(Json::of_num(rotation.x)); r.push(Json::of_num(rotation.y));
  r.push(Json::of_num(rotation.z)); r.push(Json::of_num(rotation.w));
  j["rotation_quat_xyzw"] = r;
  Json s = Json::array();
  s.push(Json::of_num(scale.x)); s.push(Json::of_num(scale.y)); s.push(Json::of_num(scale.z));
  j["scale"] = s;
}
Json Node3D::props() const { Json j = Json::object(); fill_props(j); return j; }
void Node3D::set_prop(const std::string& k, const Json& v) {
  if (k == "name" || k == "enabled") { Node::set_prop(k, v); return; }
  auto set3 = [](Vec3& dst, const Json& a) {
    if (a.is_arr() && a.arr.size() >= 3) dst = Vec3{(float)a.arr[0].num, (float)a.arr[1].num, (float)a.arr[2].num};
  };
  if (k == "position") { set3(position, v); invalidate(); }
  else if (k == "scale") { set3(scale, v); invalidate(); }
  else if (k == "rotation_euler_deg") {
    if (v.is_arr() && v.arr.size() >= 3) {
      rotation = Quat::from_euler((float)v.arr[0].num * PI / 180.0f,
                                  (float)v.arr[1].num * PI / 180.0f,
                                  (float)v.arr[2].num * PI / 180.0f);
      invalidate();
    }
  } else if (k == "rotation_quat_xyzw") {
    if (v.is_arr() && v.arr.size() >= 4) {
      rotation = Quat{(float)v.arr[0].num, (float)v.arr[1].num,
                      (float)v.arr[2].num, (float)v.arr[3].num}.normalized();
      invalidate();
    }
  }
}
std::vector<std::string> Node3D::prop_names() const {
  return {"name", "type", "enabled", "position", "rotation_euler_deg",
          "rotation_quat_xyzw", "scale"};
}

// ── MeshInstance3D ──
AABB MeshInstance3D_world_aabb(const MeshInstance3D& mi) {
  AABB b = mi.mesh ? mi.mesh->bounds : AABB();
  if (b.empty) return b;
  Mat4 w = mi.world_matrix();
  AABB out;
  Vec3 corners[8] = {{b.mn.x,b.mn.y,b.mn.z},{b.mx.x,b.mn.y,b.mn.z},{b.mn.x,b.mx.y,b.mn.z},
                     {b.mx.x,b.mx.y,b.mn.z},{b.mn.x,b.mn.y,b.mx.z},{b.mx.x,b.mn.y,b.mx.z},
                     {b.mn.x,b.mx.y,b.mx.z},{b.mx.x,b.mx.y,b.mx.z}};
  for (auto& c : corners) out.grow(w.transform_point(c));
  return out;
}
Json MeshInstance3D::props() const {
  Json j = Json::object();
  Node3D::fill_props(j);
  Json c = Json::array();
  c.push(Json::of_num(color.x)); c.push(Json::of_num(color.y)); c.push(Json::of_num(color.z));
  j["color"] = c;
  j["visible"] = Json::of_bool(visible);
  j["static_body"] = Json::of_bool(static_body);
  j["body"] = Json::of_bool(body);
  j["mesh_class"] = Json::of_str(mesh_class);
  j["mesh_name"] = Json::of_str(mesh ? mesh->name : "");
  j["mesh_tris"] = Json::of_num(mesh ? mesh->idx.size() / 3 : 0);
  Json v = Json::array();
  v.push(Json::of_num(velocity.x)); v.push(Json::of_num(velocity.y)); v.push(Json::of_num(velocity.z));
  j["velocity"] = v;
  return j;
}
void MeshInstance3D::set_prop(const std::string& k, const Json& v) {
  if (k == "color") {
    if (v.is_arr() && v.arr.size() >= 3)
      color = Vec3{(float)v.arr[0].num, (float)v.arr[1].num, (float)v.arr[2].num};
  } else if (k == "visible") visible = v.as_bool(visible);
  else if (k == "static_body") static_body = v.as_bool(static_body);
  else if (k == "body") body = v.as_bool(body);
  else if (k == "mesh_class") mesh_class = v.as_str(mesh_class);
  else if (k == "velocity") {
    if (v.is_arr() && v.arr.size() >= 3)
      velocity = Vec3{(float)v.arr[0].num, (float)v.arr[1].num, (float)v.arr[2].num};
  } else Node3D::set_prop(k, v);
}
std::vector<std::string> MeshInstance3D::prop_names() const {
  return {"name", "type", "enabled", "position", "rotation_euler_deg", "scale", "color",
          "visible", "static_body", "body", "mesh_class", "velocity"};
}

// ── Camera3D ──
Json Camera3D::props() const {
  Json j = Json::object();
  Node3D::fill_props(j);
  j["fov_deg"] = Json::of_num(fov_deg);
  j["near_z"] = Json::of_num(near_z);
  j["far_z"] = Json::of_num(far_z);
  j["active"] = Json::of_bool(active);
  return j;
}
void Camera3D::set_prop(const std::string& k, const Json& v) {
  if (k == "fov_deg") fov_deg = (float)v.as_num(fov_deg);
  else if (k == "near_z") near_z = (float)v.as_num(near_z);
  else if (k == "far_z") far_z = (float)v.as_num(far_z);
  else if (k == "active") active = v.as_bool(active);
  else Node3D::set_prop(k, v);
}
std::vector<std::string> Camera3D::prop_names() const {
  return {"name", "position", "rotation_euler_deg", "fov_deg", "near_z", "far_z", "active"};
}

// ── Light3D ──
Json Light3D::props() const {
  Json j = Json::object();
  Node3D::fill_props(j);
  Json c = Json::array();
  c.push(Json::of_num(color.x)); c.push(Json::of_num(color.y)); c.push(Json::of_num(color.z));
  j["color"] = c;
  j["intensity"] = Json::of_num(intensity);
  j["active"] = Json::of_bool(active);
  return j;
}
void Light3D::set_prop(const std::string& k, const Json& v) {
  if (k == "color") {
    if (v.is_arr() && v.arr.size() >= 3)
      color = Vec3{(float)v.arr[0].num, (float)v.arr[1].num, (float)v.arr[2].num};
  } else if (k == "intensity") intensity = (float)v.as_num(intensity);
  else if (k == "active") active = v.as_bool(active);
  else Node3D::set_prop(k, v);
}
std::vector<std::string> Light3D::prop_names() const {
  return {"name", "position", "rotation_euler_deg", "color", "intensity", "active"};
}

// ── Animator ──
void Animator::update(float dt, double time_sec) {
  (void)time_sec;
  if (!playing || track.size() < 2) return;
  time += dt;
  if (time > length) time = loop ? std::fmod(time, length) : length;
  size_t i = 0;
  while (i + 1 < track.size() && track[i + 1].t < time) ++i;
  const Keyframe& a = track[i];
  const Keyframe& b = track[i + 1];
  float span = std::max(b.t - a.t, 1e-5f);
  float t = std::clamp((time - a.t) / span, 0.0f, 1.0f);
  auto tgt = target_path.empty() ? std::dynamic_pointer_cast<Node3D>(parent.lock())
                                 : std::dynamic_pointer_cast<Node3D>(find(target_path));
  if (!tgt) return;
  tgt->position = a.pos + (b.pos - a.pos) * t;
  tgt->rotation = Quat::slerp(a.rot, b.rot, t).normalized();
  tgt->scale = a.scl + (b.scl - a.scl) * t;
  tgt->invalidate();
}
Json Animator::props() const {
  Json j = Json::object();
  Node3D::fill_props(j);
  j["target_path"] = Json::of_str(target_path);
  j["length"] = Json::of_num(length);
  j["time"] = Json::of_num(time);
  j["playing"] = Json::of_bool(playing);
  j["loop"] = Json::of_bool(loop);
  j["anim_class"] = Json::of_str(anim_class);
  j["keyframes"] = Json::of_num(track.size());
  return j;
}
void Animator::set_prop(const std::string& k, const Json& v) {
  if (k == "target_path") target_path = v.as_str(target_path);
  else if (k == "playing") playing = v.as_bool(playing);
  else if (k == "loop") loop = v.as_bool(loop);
  else if (k == "anim_class") anim_class = v.as_str(anim_class);
  else if (k == "keyframes" && v.is_arr()) {
    track.clear();
    for (auto& kf : v.arr) {
      auto arr3 = [](const Json& a, Vec3 def) {
        if (a.is_arr() && a.arr.size() >= 3)
          return Vec3{(float)a.arr[0].num, (float)a.arr[1].num, (float)a.arr[2].num};
        return def;
      };
      Keyframe k2;
      double t_val = 0;
      if (auto tv = kf.find("t")) t_val = tv->as_num(0);
      k2.t = (float)t_val;
      k2.pos = arr3(kf.find("pos") ? *kf.find("pos") : Json(), Vec3{});
      if (auto q = kf.find("rot_quat_xyzw"); q && q->is_arr() && q->arr.size() >= 4)
        k2.rot = Quat{(float)q->arr[0].num, (float)q->arr[1].num,
                      (float)q->arr[2].num, (float)q->arr[3].num}.normalized();
      k2.scl = arr3(kf.find("scale") ? *kf.find("scale") : Json(), Vec3{1, 1, 1});
      track.push_back(k2);
      if (k2.t > length) length = k2.t;
    }
    time = 0;
  } else Node3D::set_prop(k, v);
}
std::vector<std::string> Animator::prop_names() const {
  return {"target_path", "length", "time", "playing", "loop", "anim_class", "keyframes"};
}

// ── SceneTree ──
SceneTree::SceneTree() {
  root = std::make_shared<Node>("SceneTree");
  root->name = "root";
}

NodePtr SceneTree::make(const std::string& type) {
  NodePtr n;
  if (type == "Node") n = std::make_shared<Node>("Node");
  else if (type == "Node3D") n = std::make_shared<Node3D>("Node3D");
  else if (type == "MeshInstance3D") n = std::make_shared<MeshInstance3D>();
  else if (type == "Camera3D") n = std::make_shared<Camera3D>();
  else if (type == "Light3D") n = std::make_shared<Light3D>();
  else if (type == "Animator") n = std::make_shared<Animator>();
  if (n) n->id = next_id++;
  return n;
}

std::vector<std::shared_ptr<MeshInstance3D>> SceneTree::mesh_instances() const {
  std::vector<std::shared_ptr<MeshInstance3D>> out;
  std::function<void(const NodePtr&)> walk = [&](const NodePtr& n) {
    if (auto m = std::dynamic_pointer_cast<MeshInstance3D>(n)) out.push_back(m);
    for (auto& c : n->children) walk(c);
  };
  walk(root);
  return out;
}
std::vector<std::shared_ptr<Camera3D>> SceneTree::cameras() const {
  std::vector<std::shared_ptr<Camera3D>> out;
  std::function<void(const NodePtr&)> walk = [&](const NodePtr& n) {
    if (auto m = std::dynamic_pointer_cast<Camera3D>(n)) out.push_back(m);
    for (auto& c : n->children) walk(c);
  };
  walk(root);
  return out;
}
std::vector<std::shared_ptr<Light3D>> SceneTree::lights() const {
  std::vector<std::shared_ptr<Light3D>> out;
  std::function<void(const NodePtr&)> walk = [&](const NodePtr& n) {
    if (auto m = std::dynamic_pointer_cast<Light3D>(n)) out.push_back(m);
    for (auto& c : n->children) walk(c);
  };
  walk(root);
  return out;
}
std::vector<std::shared_ptr<Animator>> SceneTree::animators() const {
  std::vector<std::shared_ptr<Animator>> out;
  std::function<void(const NodePtr&)> walk = [&](const NodePtr& n) {
    if (auto m = std::dynamic_pointer_cast<Animator>(n)) out.push_back(m);
    for (auto& c : n->children) walk(c);
  };
  walk(root);
  return out;
}

Json SceneTree::tree_json() const {
  std::function<Json(const NodePtr&)> ser = [&](const NodePtr& n) -> Json {
    Json j = Json::object();
    j["name"] = Json::of_str(n->name);
    j["type"] = Json::of_str(n->type);
    if (!n->children.empty()) {
      Json a = Json::array();
      for (auto& c : n->children) a.push(ser(c));
      j["children"] = a;
    }
    return j;
  };
  return ser(root);
}

Json SceneTree::to_json() const {
  std::function<Json(const NodePtr&)> ser = [&](const NodePtr& n) -> Json {
    Json j = n->props();
    if (!n->children.empty()) {
      Json a = Json::array();
      for (auto& c : n->children) a.push(ser(c));
      j["children"] = a;
    }
    return j;
  };
  Json out = Json::object();
  out["format"] = Json::of_str("agentforge-scene");
  out["version"] = Json::of_num(1);
  out["time"] = Json::of_num(time);
  Json sc = Json::object();
  for (auto& [k, v] : scores) sc[k] = Json::of_num(v);
  out["scores"] = sc;
  out["root"] = ser(root);
  return out;
}

bool SceneTree::from_json(const Json& j, std::string* err) {
  auto r = j.find("root");
  if (!r || !r->is_obj()) { if (err) *err = "no root object"; return false; }
  std::function<bool(const Json&, const NodePtr&)> build =
      [&](const Json& jn, const NodePtr& parent) -> bool {
    std::string type = jn.find("type") ? jn.find("type")->as_str("Node") : "Node";
    // корень сериализованной сцены имеет тип "SceneTree" → это наш root,
    // новая нода не создаётся (иначе был бы дубль имени root)
    bool is_root_node = (type == "SceneTree") || (parent.get() == root.get() &&
        jn.find("name") && jn.find("name")->as_str() == root->name);
    NodePtr n = parent;
    if (!is_root_node) {
      n = make(type);
      if (!n) { if (err) *err = "unknown node type: " + type; return false; }
      n->name = jn.find("name") ? jn.find("name")->as_str("") : "";
      if (n->name.empty()) n->name = type + std::to_string(next_id);
      if (parent->child_by_name(n->name)) {
        if (err) *err = "duplicate child name '" + n->name + "' under '" + parent->name + "'";
        return false;
      }
      parent->add(n);
    }
    if (auto props = jn.find("props"); props && props->is_obj())
      for (auto& [k, v] : props->obj) n->set_prop(k, v);
    // плоские поля (как пишет to_json) тоже применяем — set_prop игнорирует неизвестные
    for (auto& [k, v] : jn.obj) {
      if (k == "children" || k == "type" || k == "name" || k == "props") continue;
      n->set_prop(k, v);
    }
    if (auto ch = jn.find("children"); ch && ch->is_arr())
      for (auto& c : ch->arr)
        if (!build(c, n)) return false;
    return true;
  };
  if (auto t = j.find("time")) time = t->as_num(0);
  if (auto sc = j.find("scores"); sc && sc->is_obj())
    for (auto& [k, v] : sc->obj) scores[k] = v.as_num(0);
  if (!build(*r, root)) return false;
  return true;
}

} // namespace af
