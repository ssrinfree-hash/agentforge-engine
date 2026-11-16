// AgentForge — benchmark: пропускная способность растера, протокола,
// построения сцены, физики, поиска ассетов.
#include "af/agent.h"
#include <chrono>
#include <cstdio>
#include <algorithm>

namespace af {

static double now_ms2() {
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

Json Session::benchmark() {
  Json out = Json::object();
  out["engine"] = Json::of_str("AgentForge " + std::string(ENGINE_VERSION));
  Json b = Json::object();

  // 1. Рендер: сфера высокой плотности, 640x360
    std::fprintf(stderr, "[bench] raster\n");
  {
    tree = SceneTree();
    auto cam = std::dynamic_pointer_cast<Camera3D>(tree.make("Camera3D"));
    cam->name = "Cam";
    tree.root->add(cam);
    for (int i = 0; i < 8; ++i) {
      auto mi = std::dynamic_pointer_cast<MeshInstance3D>(tree.make("MeshInstance3D"));
      mi->name = "Sphere" + std::to_string(i);
      mi->mesh = make_sphere(0.6f, 28);
      mi->position = {float(i % 4) * 1.6f - 2.4f, 0.6f, -6.0f - float(i / 4) * 2.0f};
      tree.root->add(mi);
    }
    renderer.set_viewport(640, 360);
    uint32_t tris = 0;
    auto t0 = now_ms2();
    const int N = 20;
    for (int i = 0; i < N; ++i) {
      Frame f = renderer.render(tree, cam, Vec3{0.4f, -1, -0.3f}, Vec3{1, 1, 1}, 1.0f);
      tris = f.drawn_tris;
    }
    double dt = now_ms2() - t0;
    double mtri_per_s = double(tris) * N / (dt * 1000.0);
    b["raster_mtris_per_s"] = Json::of_num(mtri_per_s);
    b["raster_ms_per_frame"] = Json::of_num(dt / N);
    b["raster_tris_per_frame"] = Json::of_num(tris);
    b["raster_resolution"] = Json::of_str("640x360");
  }

  // 2. Протокол: 2000 вызовов scene.tree на сцене из 100 нод
    std::fprintf(stderr, "[bench] protocol\n");
  {
    dispatch([]() {
      Json j = Json::object(); j["cmd"] = Json::of_str("scene.new"); return j;
    }());
    for (int i = 0; i < 100; ++i) {
      Json req = Json::object();
      req["cmd"] = Json::of_str("node.add");
      req["parent"] = Json::of_str("root");
      Json n = Json::object();
      n["type"] = Json::of_str("Node3D");
      n["name"] = Json::of_str("N" + std::to_string(i));
      req["node"] = n;
      dispatch(req);
    }
    auto t0 = now_ms2();
    const int N = 2000;
    Json q = Json::object(); q["cmd"] = Json::of_str("scene.tree");
    for (int i = 0; i < N; ++i) dispatch(q);
    double dt = now_ms2() - t0;
    b["protocol_us_per_call"] = Json::of_num(dt * 1000.0 / N);
    b["protocol_calls_per_s"] = Json::of_num(N / (dt / 1000.0));
    b["protocol_scene_nodes"] = Json::of_num(100);
  }

  // 3. Построение сцены: 1000 node.add
    std::fprintf(stderr, "[bench] scene_build\n");
  {
    dispatch([]() {
      Json j = Json::object(); j["cmd"] = Json::of_str("scene.new"); return j;
    }());
    auto t0 = now_ms2();
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
      Json req = Json::object();
      req["cmd"] = Json::of_str("node.add");
      req["parent"] = Json::of_str("root");
      Json n = Json::object();
      n["type"] = Json::of_str("MeshInstance3D");
      n["name"] = Json::of_str("B" + std::to_string(i));
      Json mesh = Json::object();
      mesh["primitive"] = Json::of_str("box");
      n["mesh"] = mesh;
      req["node"] = n;
      dispatch(req);
    }
    double dt = now_ms2() - t0;
    b["scene_build_us_per_node"] = Json::of_num(dt * 1000.0 / N);
    b["scene_build_nodes_per_s"] = Json::of_num(N / (dt / 1000.0));
  }

  // 4. Физика: 20 динамических тел, 600 шагов
    std::fprintf(stderr, "[bench] physics\n");
  {
    dispatch([]() {
      Json j = Json::object(); j["cmd"] = Json::of_str("scene.new"); return j;
    }());
    Json ground = Json::object();
    ground["cmd"] = Json::of_str("node.add");
    ground["parent"] = Json::of_str("root");
    Json gn = Json::object();
    gn["type"] = Json::of_str("MeshInstance3D");
    gn["name"] = Json::of_str("Ground");
    Json gm = Json::object();
    gm["primitive"] = Json::of_str("plane");
    gm["size"] = []() { Json a = Json::array(); a.push(Json::of_num(50)); return a; }();
    gn["mesh"] = gm;
    Json gp = Json::object();
    gp["static_body"] = Json::of_bool(true);
    gn["props"] = gp;
    ground["node"] = gn;
    dispatch(ground);
    for (int i = 0; i < 20; ++i) {
      Json req = Json::object();
      req["cmd"] = Json::of_str("node.add");
      req["parent"] = Json::of_str("root");
      Json n = Json::object();
      n["type"] = Json::of_str("MeshInstance3D");
      n["name"] = Json::of_str("Dyn" + std::to_string(i));
      Json mesh = Json::object();
      mesh["primitive"] = Json::of_str("sphere");
      mesh["radius"] = Json::of_num(0.4);
      n["mesh"] = mesh;
      Json props = Json::object();
      props["body"] = Json::of_bool(true);
      Json pos = Json::array();
      pos.push(Json::of_num(float(i % 5) * 1.2f - 2.4f));
      pos.push(Json::of_num(2.0f + float(i / 5)));
      pos.push(Json::of_num(0.0f));
      props["position"] = pos;
      n["props"] = props;
      req["node"] = n;
      dispatch(req);
    }
    auto t0 = now_ms2();
    Json step = Json::object();
    step["cmd"] = Json::of_str("sim.step");
    step["frames"] = Json::of_num(600);
    Json r = dispatch(step);
    double dt = now_ms2() - t0;
    b["physics_ms_per_600steps_20bodies"] = Json::of_num(dt);
    b["physics_steps_per_s"] = Json::of_num(600.0 / (dt / 1000.0));
  }

  // 5. Поиск ассетов (если веса загружены или есть каталог)
    std::fprintf(stderr, "[bench] assets\n");
  {
    if (!catalog.entries.empty()) {
      auto t0 = now_ms2();
      const int N = 200;
      Json q = Json::object();
      q["cmd"] = Json::of_str("asset.search");
      q["query"] = Json::of_str("chair");
      for (int i = 0; i < N; ++i) dispatch(q);
      double dt = now_ms2() - t0;
      b["asset_search_us_per_query"] = Json::of_num(dt * 1000.0 / N);
      b["asset_catalog_entries"] = Json::of_num(catalog.entries.size());
      b["asset_model"] = Json::of_str(model.loaded() ? "trained" : "fallback");
    } else {
      b["asset_search"] = Json::of_str("catalog not loaded (use --catalog/--weights)");
    }
  }

  out["ok"] = Json::of_bool(true);
  out["benchmark"] = b;
  return out;
}

} // namespace af
