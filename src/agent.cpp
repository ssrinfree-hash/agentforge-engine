// AgentForge — реализация агент-протокола, selftest, benchmark.
#include "af/agent.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>

namespace af {

static double now_ms() {
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Session* g_active_session = nullptr;   // плагины логируют в активную сессию

static double num_field(const Json& j, const char* key, double def) {
  auto v = j.find(key);
  return v && v->is_num() ? v->num : def;
}

// ── AfApi мосты ──
int Session::api_node_add(void* s, const char* parent, const char* node_json) {
  auto* self = static_cast<Session*>(s);
  Json spec, out = Json::object();
  if (!Json::parse(node_json, spec)) return -1;
  out["cmd"] = Json::of_str("node.add");
  out["parent"] = Json::of_str(parent);
  out["node"] = spec;
  Json r = self->dispatch(out);
  return r.find("ok") && r.find("ok")->as_bool() ? 0 : -2;
}
int Session::api_node_set(void* s, const char* path, const char* props_json) {
  auto* self = static_cast<Session*>(s);
  Json props, out = Json::object();
  if (!Json::parse(props_json, props)) return -1;
  out["cmd"] = Json::of_str("node.set");
  out["path"] = Json::of_str(path);
  out["props"] = props;
  Json r = self->dispatch(out);
  return r.find("ok") && r.find("ok")->as_bool() ? 0 : -2;
}
const char* Session::api_node_get(void* s, const char* path) {
  auto* self = static_cast<Session*>(s);
  Json out = Json::object();
  out["cmd"] = Json::of_str("node.get");
  out["path"] = Json::of_str(path);
  Json r = self->dispatch(out);
  self->get_buffer = r.dump();
  return self->get_buffer.c_str();
}
int Session::api_node_remove(void* s, const char* path) {
  auto* self = static_cast<Session*>(s);
  Json out = Json::object();
  out["cmd"] = Json::of_str("node.remove");
  out["path"] = Json::of_str(path);
  Json r = self->dispatch(out);
  return r.find("ok") && r.find("ok")->as_bool() ? 0 : -2;
}
double Session::api_score_get(void* s, const char* key) {
  auto* self = static_cast<Session*>(s);
  auto it = self->tree.scores.find(key);
  return it == self->tree.scores.end() ? 0.0 : it->second;
}
void Session::api_score_set(void* s, const char* key, double v) {
  static_cast<Session*>(s)->tree.scores[key] = v;
}
void Session::api_log(const char* msg) {
  if (msg) std::fprintf(stderr, "[plugin] %s\n", msg);
  if (g_active_session && msg) {
    Json ev = Json::object();
    ev["event"] = Json::of_str("log");
    ev["msg"] = Json::of_str(msg);
    g_active_session->api_event_log_.push(ev);
  }
}
double Session::api_time(void* s) { return static_cast<Session*>(s)->tree.time; }
Json Session::api_last_events(void* s) { return static_cast<Session*>(s)->api_event_log_; }

// ── Меши: примитивы и файлы, кэш ──
std::shared_ptr<Mesh> Session::get_mesh(const Json& spec, std::string* err) {
  if (auto file = spec.find("file"); file && file->is_str()) {
    auto it = mesh_cache_.find(file->str);
    if (it != mesh_cache_.end()) return it->second;
    auto m = load_any(file->str, err);
    if (m) mesh_cache_[file->str] = m;
    return m;
  }
  if (auto prim = spec.find("primitive"); prim && prim->is_str()) {
    const std::string& p = prim->str;
    auto sz = spec.find("size");
    Vec3 s3{1, 1, 1};
    if (sz && sz->is_arr() && sz->arr.size() >= 3)
      s3 = Vec3{(float)sz->arr[0].num, (float)sz->arr[1].num, (float)sz->arr[2].num};
    float radius = spec.find("radius") ? (float)spec.find("radius")->as_num(0.5) : 0.5f;
    float height = spec.find("height") ? (float)spec.find("height")->as_num(1.8) : 1.8f;
    if (p == "box") return make_box(s3);
    if (p == "plane") return make_plane(sz && sz->is_arr() && !sz->arr.empty() ? (float)sz->arr[0].num : 10.0f);
    if (p == "sphere") return make_sphere(radius);
    if (p == "capsule") return make_capsule(radius, height);
    if (err) *err = "unknown primitive: " + p;
    return nullptr;
  }
  if (err) *err = "mesh spec must have {file:...} or {primitive:...}";
  return nullptr;
}

Json Session::node_props_json(const NodePtr& n) {
  Json j = n->props();
  Json children = Json::array();
  for (auto& c : n->children) children.push(node_props_json(c));
  if (!children.arr.empty()) j["children"] = children;
  return j;
}

// ── Схема (self-describing движок — агенту не нужна внешняя документация) ──
Json Session::node_types_schema() const {
  Session tmp;
  const char* types[] = {"Node", "Node3D", "MeshInstance3D", "Camera3D", "Light3D", "Animator"};
  Json arr = Json::array();
  for (const char* t : types) {
    NodePtr n = tmp.tree.make(t);
    Json j = Json::object();
    j["type"] = Json::of_str(t);
    Json props = Json::array();
    for (auto& p : n->prop_names()) props.push(Json::of_str(p));
    j["props"] = props;
    arr.push(j);
  }
  return arr;
}

Json Session::schema() const {
  Json j = Json::object();
  j["engine"] = Json::of_str("AgentForge");
  j["version"] = Json::of_str(ENGINE_VERSION);
  j["protocol"] = Json::of_str("json-rpc-lines: каждая строка stdin — один JSON-запрос {\"cmd\":...}; каждый JSON-объект stdout — ответ");
  j["node_types"] = node_types_schema();
  Json mesh_spec = Json::object();
  mesh_spec["primitive"] = Json::of_str("box|plane|sphere|capsule");
  mesh_spec["size"] = Json::of_str("[x,y,z] для box; [s] для plane");
  mesh_spec["radius"] = Json::of_str("для sphere/capsule");
  mesh_spec["height"] = Json::of_str("для capsule");
  mesh_spec["file"] = Json::of_str("путь к .obj/.off (OBJ с mtl Kd)");
  j["mesh_spec"] = mesh_spec;
  Json cmds = Json::array();
  for (const char* c : {"ping", "schema", "scene.new", "scene.load", "scene.save", "scene.tree",
                        "node.add", "node.set", "node.get", "node.remove",
                        "sim.step", "sim.reset", "raycast",
                        "frame.capture", "asset.search", "scores.set",
                        "plugin.load", "plugin.call", "selftest", "benchmark", "quit"})
    cmds.push(Json::of_str(c));
  j["commands"] = cmds;
  Json examples = Json::object();
  {
    Json e1 = Json::object();
    e1["cmd"] = Json::of_str("node.add");
    e1["parent"] = Json::of_str("root");
    e1["node"] = [](){
      Json n = Json::object();
      n["type"] = Json::of_str("MeshInstance3D");
      n["name"] = Json::of_str("Crate");
      Json mesh = Json::object();
      mesh["primitive"] = Json::of_str("box");
      mesh["size"] = [](){ Json a = Json::array(); a.push(Json::of_num(1)); a.push(Json::of_num(1)); a.push(Json::of_num(1)); return a; }();
      n["mesh"] = mesh;
      Json props = Json::object();
      Json pos = Json::array();
      pos.push(Json::of_num(0)); pos.push(Json::of_num(0.5)); pos.push(Json::of_num(0));
      props["position"] = pos;
      Json col = Json::array();
      col.push(Json::of_num(0.8)); col.push(Json::of_num(0.4)); col.push(Json::of_num(0.1));
      props["color"] = col;
      props["static_body"] = Json::of_bool(true);
      n["props"] = props;
      return n;
    }();
    examples["node.add"] = e1;
    Json e2 = Json::object();
    e2["cmd"] = Json::of_str("frame.capture");
    e2["width"] = Json::of_num(640);
    e2["height"] = Json::of_num(360);
    e2["save"] = Json::of_str("/tmp/frame.ppm");
    examples["frame.capture"] = e2;
  }
  j["examples"] = examples;
  return j;
}

// ── Кадр ──
Json Session::capture_frame(int w, int h, const std::string& save_path) {
  if (w > 0 && (w != viewport_w || h != viewport_h)) {
    viewport_w = w; viewport_h = h;
  }
  renderer.set_viewport(viewport_w, viewport_h);
  auto cam = [&]() -> std::shared_ptr<Camera3D> {
    for (auto& c : tree.cameras())
      if (c->active) return c;
    return nullptr;
  }();
  Json out = Json::object();
  if (!cam) {
    out["ok"] = Json::of_bool(false);
    out["error"] = Json::of_str("no active Camera3D in scene");
    return out;
  }
  Vec3 ldir{0.4f, -1.0f, -0.3f};
  Vec3 lcol{1, 1, 1};
  float lint = 1.0f;
  for (auto& l : tree.lights()) {
    if (!l->active) continue;
    ldir = l->direction();
    lcol = l->color;
    lint = l->intensity;
    break;
  }
  Frame frame = renderer.render(tree, cam, ldir, lcol, lint);
  FrameReport rep = Renderer::report(tree, frame, cam);
  last_frame_report = rep.to_json(tree);
  if (!save_path.empty()) {
    frame.save_ppm(save_path);
    last_frame_report["saved_ppm"] = Json::of_str(save_path);
  }
  out["ok"] = Json::of_bool(true);
  out["frame"] = last_frame_report;
  return out;
}

// ── Dispatch ──
Json Session::dispatch(const Json& req) {
  Json resp = Json::object();
  auto ok = [&](bool v) { resp["ok"] = Json::of_bool(v); };
  auto err = [&](const std::string& e) { resp["error"] = Json::of_str(e); ok(false); };
  auto cmdv = req.find("cmd");
  if (!cmdv || !cmdv->is_str()) { err("no cmd field"); return resp; }
  const std::string cmd = cmdv->str;
  try {
    if (cmd == "ping") {
      ok(true);
      resp["engine"] = Json::of_str("AgentForge");
      resp["version"] = Json::of_str(ENGINE_VERSION);
      resp["time"] = Json::of_num(tree.time);
      return resp;
    }
    if (cmd == "schema") { ok(true); resp["schema"] = schema(); return resp; }
    if (cmd == "scene.new") {
      tree = SceneTree();
      api_event_log_ = Json::array();
      mesh_cache_.clear();
      ok(true);
      resp["scene"] = tree.tree_json();
      return resp;
    }
    if (cmd == "scene.load") {
      std::string text, e2;
      if (auto p = req.find("path"); p && p->is_str()) {
        std::ifstream f(p->str);
        if (!f) { err("cannot open " + p->str); return resp; }
        std::stringstream ss; ss << f.rdbuf();
        text = ss.str();
      } else if (auto j = req.find("scene"); j && (j->is_obj())) {
        text = j->dump();
      } else { err("need path or scene"); return resp; }
      Json sj;
      if (!Json::parse(text, sj, &e2)) { err("bad scene json: " + e2); return resp; }
      tree = SceneTree();
      if (!tree.from_json(sj, &e2)) { err("scene load failed: " + e2); return resp; }
      ok(true);
      resp["scene"] = tree.tree_json();
      return resp;
    }
    if (cmd == "scene.save") {
      auto p = req.find("path");
      if (!p || !p->is_str()) { err("need path"); return resp; }
      std::ofstream f(p->str);
      f << tree.to_json().dump(2);
      ok(true);
      resp["path"] = *p;
      return resp;
    }
    if (cmd == "scene.tree") { ok(true); resp["scene"] = tree.tree_json(); return resp; }
    if (cmd == "node.add") {
      auto nspec = req.find("node");
      if (!nspec || !nspec->is_obj()) { err("need node object"); return resp; }
      std::string parent_path = req.find("parent") ? req.find("parent")->as_str("root") : "root";
      NodePtr parent = parent_path == "root" ? tree.root : tree.find(parent_path);
      if (!parent) { err("parent not found: " + parent_path); return resp; }
      std::string type = nspec->find("type") ? nspec->find("type")->as_str("Node") : "Node";
      NodePtr n = tree.make(type);
      if (!n) { err("unknown type: " + type + " (see schema)"); return resp; }
      std::string name = nspec->find("name") ? nspec->find("name")->as_str("") : "";
      if (name.empty()) name = type + std::to_string(tree.next_id);
      if (parent->child_by_name(name)) { err("duplicate name under parent: " + name); return resp; }
      n->name = name;
      if (auto props = nspec->find("props"); props && props->is_obj())
        for (auto& [k, v] : props->obj) n->set_prop(k, v);
      // сахар: mesh/transform на верхнем уровне node-объекта
      for (const char* key : {"position", "scale", "rotation_euler_deg", "color",
                              "visible", "body", "static_body", "mesh_class", "velocity"})
        if (auto v = nspec->find(key)) n->set_prop(key, *v);
      if (auto mv = nspec->find("mesh"); mv && mv->is_obj()) {
        auto mi = std::dynamic_pointer_cast<MeshInstance3D>(n);
        if (!mi) { err("mesh allowed only on MeshInstance3D"); return resp; }
        std::string e2;
        auto m = get_mesh(*mv, &e2);
        if (!m) { err("mesh: " + e2); return resp; }
        mi->mesh = m;
        if (!mi->mesh_class.empty()) mi->mesh_class = mi->mesh_class;
      }
      parent->add(n);
      ok(true);
      resp["path"] = Json::of_str(n->path());
      resp["id"] = Json::of_num(n->id);
      return resp;
    }
    if (cmd == "node.set") {
      auto path = req.find("path");
      auto props = req.find("props");
      if (!path || !props || !props->is_obj()) { err("need path + props"); return resp; }
      NodePtr n = tree.find(path->str);
      if (!n) { err("node not found: " + path->str); return resp; }
      for (auto& [k, v] : props->obj) n->set_prop(k, v);
      if (auto mi = std::dynamic_pointer_cast<MeshInstance3D>(n)) {
        if (auto mv = props->find("mesh"); mv && mv->is_obj()) {
          std::string e2;
          auto m = get_mesh(*mv, &e2);
          if (!m) { err("mesh: " + e2); return resp; }
          mi->mesh = m;
        }
      }
      ok(true);
      resp["path"] = *path;
      resp["node"] = node_props_json(n);
      return resp;
    }
    if (cmd == "node.get") {
      auto path = req.find("path");
      if (!path) { err("need path"); return resp; }
      NodePtr n = tree.find(path->str);
      if (!n) { err("node not found: " + path->str); return resp; }
      ok(true);
      resp["node"] = node_props_json(n);
      return resp;
    }
    if (cmd == "node.remove") {
      auto path = req.find("path");
      if (!path) { err("need path"); return resp; }
      size_t slash = path->str.find_last_of('/');
      std::string parent_path = slash == std::string::npos ? "" : path->str.substr(0, slash);
      std::string cname = slash == std::string::npos ? path->str : path->str.substr(slash + 1);
      NodePtr parent = parent_path.empty() ? tree.root : tree.find(parent_path);
      if (!parent || !parent->remove(cname)) { err("node not found: " + path->str); return resp; }
      ok(true);
      return resp;
    }
    if (cmd == "sim.step") {
      int frames = (int)num_field(req, "frames", 1);
      float dt = (float)num_field(req, "dt", 1.0 / 60.0);
      frames = std::clamp(frames, 1, 100000);
      Json all_events = Json::array();
      for (int i = 0; i < frames; ++i) {
        tree.update(dt);
        Json pe = physics_step(tree, dt, physics);
        for (auto& e : pe.find("events")->arr) all_events.push(e);
        if (p_update) p_update(dt, tree.time);
      }
      ok(true);
      resp["frames"] = Json::of_num(frames);
      resp["time"] = Json::of_num(tree.time);
      resp["frame"] = Json::of_num(tree.frame);
      resp["events"] = all_events;
      resp["plugin_log"] = api_event_log_;    // логи плагина игровой логики
      Json sc = Json::object();
      for (auto& [k, v] : tree.scores) sc[k] = Json::of_num(v);
      resp["scores"] = sc;
      return resp;
    }
    if (cmd == "sim.reset") {
      tree.time = 0;
      tree.frame = 0;
      tree.scores.clear();
      api_event_log_ = Json::array();
      ok(true);
      return resp;
    }
    if (cmd == "raycast") {
      auto o = req.find("origin"), d = req.find("dir");
      if (!o || !d || !o->is_arr() || !d->is_arr()) { err("need origin[3], dir[3]"); return resp; }
      Vec3 origin{(float)o->arr[0].num, (float)o->arr[1].num, (float)o->arr[2].num};
      Vec3 dir{(float)d->arr[0].num, (float)d->arr[1].num, (float)d->arr[2].num};
      float max_dist = (float)num_field(req, "max_dist", 100.0);
      ok(true);
      resp["raycast"] = raycast(tree, origin, dir, max_dist);
      return resp;
    }
    if (cmd == "frame.capture") {
      int w = (int)num_field(req, "width", viewport_w);
      int h = (int)num_field(req, "height", viewport_h);
      std::string save = req.find("save") ? req.find("save")->as_str("") : "";
      return capture_frame(w, h, save);
    }
    if (cmd == "asset.search") {
      auto q = req.find("query");
      if (!q || !q->is_str()) { err("need query"); return resp; }
      int top_k = (int)num_field(req, "top_k", 10);
      ok(true);
      resp["search"] = model.search(q->str, catalog, std::clamp(top_k, 1, 100));
      return resp;
    }
    if (cmd == "scores.set") {
      auto sc = req.find("scores");
      if (!sc || !sc->is_obj()) { err("need scores object"); return resp; }
      for (auto& [k, v] : sc->obj) tree.scores[k] = v.as_num(0);
      ok(true);
      return resp;
    }
    if (cmd == "plugin.load") {
      auto p = req.find("path");
      if (!p || !p->is_str()) { err("need path"); return resp; }
      if (plugin_handle) dlclose(plugin_handle);
      plugin_handle = dlopen(p->str.c_str(), RTLD_NOW);
      if (!plugin_handle) { err(std::string("dlopen: ") + dlerror()); return resp; }
      p_init = (af_plugin_init_fn)dlsym(plugin_handle, "af_plugin_init");
      p_update = (af_plugin_update_fn)dlsym(plugin_handle, "af_plugin_update");
      p_event = (af_plugin_event_fn)dlsym(plugin_handle, "af_plugin_event");
      if (!p_init || !p_update) { err("missing af_plugin_init/af_plugin_update"); return resp; }
      AfApi api;
      api.session = this;
      api.node_add = &Session::api_node_add;
      api.node_set = &Session::api_node_set;
      api.node_get = &Session::api_node_get;
      api.node_remove = &Session::api_node_remove;
      api.score_get = &Session::api_score_get;
      api.score_set = &Session::api_score_set;
      api.log = &Session::api_log;
      api.time = &Session::api_time;
      api.last_events = &Session::api_last_events;
      g_active_session = this;
      int rc = p_init(&api);
      if (rc != 0) { err("af_plugin_init returned " + std::to_string(rc)); return resp; }
      ok(true);
      return resp;
    }
    if (cmd == "plugin.call") {
      auto a = req.find("event");
      if (!p_event) { err("no plugin loaded"); return resp; }
      std::string ev = a ? a->dump() : "{}";
      const char* r = p_event(ev.c_str());
      Json out;
      std::string e2;
      if (r && Json::parse(r, out, &e2)) { ok(true); resp["plugin"] = out; }
      else { ok(true); resp["plugin_raw"] = Json::of_str(r ? r : "(null)"); }
      return resp;
    }
    if (cmd == "selftest") { return selftest(); }
    if (cmd == "benchmark") { return benchmark(); }
    if (cmd == "quit") { ok(true); resp["bye"] = Json::of_bool(true); return resp; }
    err("unknown cmd: " + cmd + " (send {\"cmd\":\"schema\"})");
    return resp;
  } catch (const std::exception& e) {
    err(std::string("exception: ") + e.what());
    return resp;
  }
}

} // namespace af
