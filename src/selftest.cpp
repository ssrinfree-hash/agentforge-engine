// AgentForge — selftest: полный прогон внутренних проверок движка.
#include "af/agent.h"
#include <cstdio>
#include <cmath>
#include <fstream>
#include <cmath>

namespace af {

static int g_checks = 0, g_failed = 0;

static void check(bool cond, const std::string& name, Json& results) {
  ++g_checks;
  Json j = Json::object();
  j["name"] = Json::of_str(name);
  j["pass"] = Json::of_bool(cond);
  if (!cond) ++g_failed;
  results.push(j);
}

Json Session::selftest() {
  g_checks = 0; g_failed = 0;
  Json results = Json::array();
  std::fprintf(stderr, "[selftest] start\n");
#define AF_STAGE(name) std::fprintf(stderr, "[selftest] %s\n", name)

  // 1. Математика: кватернион вращает вектор вокруг оси
  AF_STAGE("math");
  {
    Quat q = Quat::from_axis_angle({0, 1, 0}, PI / 2);
    Vec3 r = q.rotate({0, 0, -1});
    check(std::fabs(r.x + 1.0f) < 1e-4f && std::fabs(r.z) < 1e-4f,
          "math.quat_rotate_y90", results);
    Mat4 m = Mat4::trs({1, 2, 3}, q, {1, 1, 1});
    Vec3 p = m.transform_point({0, 0, -2});
    check(std::fabs(p.x + 1.0f) < 1e-4f && std::fabs(p.z - 3.0f) < 1e-4f,
          "math.trs_point", results);
    Mat4 i = m * m.inverse();
    bool ok_inv = true;
    for (int c = 0; c < 4; ++c)
      for (int rr = 0; rr < 4; ++rr) {
        float e = i.at(rr, c) - (rr == c ? 1.0f : 0.0f);
        if (std::fabs(e) > 1e-3f) ok_inv = false;
      }
    check(ok_inv, "math.mat4_inverse", results);
    Quat s = Quat::slerp(Quat::identity(), q, 1.0f);
    check(std::fabs(s.w - q.w) < 1e-5f && std::fabs(s.y - q.y) < 1e-5f,
          "math.slerp_endpoints", results);
  }

  // 2. JSON roundtrip
  AF_STAGE("json");
  {
    std::string src = R"({"a":1.5,"b":[1,2,3],"c":{"d":"text\nline","e":true},"f":null})";
    Json j;
    std::string e2;
    bool ok = Json::parse(src, j, &e2);
    check(ok && std::fabs(j.find("a")->num - 1.5) < 1e-9 && j.find("c")->find("e")->b,
          "json.parse_nested", results);
    std::string out = j.dump();
    Json j2;
    check(Json::parse(out, j2, &e2) && j2.find("c")->find("d")->str == "text\nline",
          "json.dump_roundtrip", results);
    Json bad;
    check(!Json::parse("{bad", bad), "json.rejects_invalid", results);
  }

  // 3. Сцена: иерархия трансформов + сериализация
  AF_STAGE("scene");
  {
    tree = SceneTree();
    auto parent = std::dynamic_pointer_cast<Node3D>(tree.make("Node3D"));
    parent->name = "Parent";
    parent->position = {5, 0, 0};
    tree.root->add(parent);
    auto child = std::dynamic_pointer_cast<Node3D>(tree.make("Node3D"));
    child->name = "Child";
    child->position = {0, 0, -2};
    parent->add(child);
    Vec3 w = std::dynamic_pointer_cast<Node3D>(child)->world_matrix().transform_point(Vec3{});
    check(std::fabs(w.x - 5) < 1e-5f && std::fabs(w.z + 2) < 1e-5f,
          "scene.hierarchy_transform", results);
    Json sj = tree.to_json();
    std::string dump = sj.dump();
    SceneTree t2;
    Json j2;
    Json::parse(dump, j2);
    std::string e2;
    bool ok2 = t2.from_json(j2, &e2);
    auto found = ok2 ? t2.find("Parent/Child") : nullptr;
    check(found != nullptr, "scene.json_roundtrip", results);
  }

  // 4. Рендер: куб перед камерой рисуется, отчёт видит bbox
  AF_STAGE("render");
  {
    tree = SceneTree();
    auto mi = std::dynamic_pointer_cast<MeshInstance3D>(tree.make("MeshInstance3D"));
    mi->name = "Box";
    mi->mesh = make_box({1, 1, 1});
    mi->position = {0, 0, -5};
    mi->mesh_class = "crate";
    tree.root->add(mi);
    auto cam = std::dynamic_pointer_cast<Camera3D>(tree.make("Camera3D"));
    cam->name = "Cam";
    tree.root->add(cam);
    renderer.set_viewport(160, 120);
    Frame f = renderer.render(tree, cam, Vec3{0.4f, -1, -0.3f}, Vec3{1, 1, 1}, 1.0f);
    uint8_t center[4];
    for (int i = 0; i < 4; ++i) center[i] = f.color[((60 * 160 + 80) * 4) + i];
    bool drawn = !(center[0] == 26 && center[1] == 31 && center[2] == 41);  // не фоновый цвет
    check(drawn && f.drawn_tris > 0, "render.box_draws_pixels", results);
    FrameReport rep = Renderer::report(tree, f, cam);
    check(rep.objects.size() == 1 && rep.objects[0].visible &&
          rep.objects[0].mesh_class == "crate",
          "render.frame_report_bbox", results);
    // камера развернулась на 180° → объект полностью за спиной
    cam->rotation = Quat::from_axis_angle({0, 1, 0}, PI);
    cam->invalidate();
    f = renderer.render(tree, cam, Vec3{0.4f, -1, -0.3f}, Vec3{1, 1, 1}, 1.0f);
    rep = Renderer::report(tree, f, cam);
    check(!rep.objects[0].visible, "render.backface_visibility", results);
  }

  // 5. Физика: мяч падает на землю, событие ground_hit
  AF_STAGE("physics");
  {
    tree = SceneTree();
    auto ball = std::dynamic_pointer_cast<MeshInstance3D>(tree.make("MeshInstance3D"));
    ball->name = "Ball";
    ball->mesh = make_sphere(0.5f);
    ball->position = {0, 10, 0};
    ball->body = true;
    tree.root->add(ball);
    bool hit_ground = false;
    for (int i = 0; i < 600 && !hit_ground; ++i) {
      tree.update(1.0f / 60.0f);
      Json pe = physics_step(tree, 1.0f / 60.0f, physics);
      for (auto& ev : pe.find("events")->arr)
        if (ev.find("event")->str == "ground_hit") hit_ground = true;
    }
    check(hit_ground && std::fabs(ball->position.y - 0.5f) < 0.2f,
          "physics.gravity_ground", results);
  }

  // 6. Анимация: поворот на 90° за 1 с
  AF_STAGE("anim");
  {
    tree = SceneTree();
    auto spinner = std::dynamic_pointer_cast<Node3D>(tree.make("Node3D"));
    spinner->name = "Spinner";
    auto anim = std::dynamic_pointer_cast<Animator>(tree.make("Animator"));
    anim->name = "Spin";
    anim->target_path = "Spinner";     // таргет ищется среди детей Animator
    anim->loop = false;
    Json kf = Json::array();
    auto push_kf = [&](float t, float deg) {
      Json k = Json::object();
      k["t"] = Json::of_num(t);
      Json p = Json::array();
      p.push(Json::of_num(0)); p.push(Json::of_num(0)); p.push(Json::of_num(0));
      k["pos"] = p;
      Quat q = Quat::from_axis_angle({0, 1, 0}, deg * PI / 180.0f);
      Json qj = Json::array();
      qj.push(Json::of_num(q.x)); qj.push(Json::of_num(q.y));
      qj.push(Json::of_num(q.z)); qj.push(Json::of_num(q.w));
      k["rot_quat_xyzw"] = qj;
      Json s = Json::array();
      s.push(Json::of_num(1)); s.push(Json::of_num(1)); s.push(Json::of_num(1));
      k["scale"] = s;
      kf.push(k);
    };
    push_kf(0.0f, 0.0f);
    push_kf(1.0f, 90.0f);
    anim->set_prop("keyframes", kf);
    anim->add(spinner);              // таргет — ребёнок Animator
    tree.root->add(anim);
    for (int i = 0; i < 60; ++i) tree.update(1.0f / 60.0f);
    float z_after = spinner->rotation.rotate({0, 0, -1}).x;
    check(std::fabs(z_after + 1.0f) < 0.05f, "anim.keyframe_rotation", results);
  }

  // 7. Ассеты: OBJ на диске + примитивы
  AF_STAGE("assets");
  {
    std::string tmp = "/tmp/af_test_cube.obj";
    std::ofstream f(tmp);
    f << "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
      << "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
      << "f 1 2 3\nf 1 3 4\nf 5 6 7\nf 5 7 8\n";
    f.close();
    std::string e2;
    auto m = load_obj(tmp, &e2);
    check(m && m->pos.size() == 8 && m->idx.size() == 12, "assets.load_obj", results);
    check(make_box({1, 1, 1})->idx.size() == 36 && make_sphere(1.0f)->pos.size() > 100,
          "assets.primitives", results);
    AssetCatalog cat;
    CatalogEntry e3;
    e3.id = "chair_0001"; e3.cls = "chair"; e3.name = "office chair";
    cat.entries.push_back(e3);
    Json r = model.search("chair", cat, 5);   // без весов — fallback substring
    check(r.find("results")->arr.size() == 1 &&
          r.find("results")->arr[0].find("class")->str == "chair",
          "assets.search_fallback", results);
  }

  // 8. Протокол: диспетчер end-to-end
  AF_STAGE("protocol");
  {
    tree = SceneTree();
    auto mkreq = [](const char* cmd) {
      Json j = Json::object();
      j["cmd"] = Json::of_str(cmd);
      return j;
    };
    std::fprintf(stderr, "[p8] scene.new\n"); Json r1 = dispatch(mkreq("scene.new"));
    std::fprintf(stderr, "[p8] schema\n"); Json r2 = dispatch(mkreq("schema"));
    bool schema_ok = r2.find("schema")->find("node_types")->arr.size() == 6;
    check(r1.find("ok")->b && schema_ok, "protocol.dispatch_schema", results);
    Json add = Json::object();
    add["cmd"] = Json::of_str("node.add");
    add["parent"] = Json::of_str("root");
    Json node = Json::object();
    node["type"] = Json::of_str("MeshInstance3D");
    node["name"] = Json::of_str("T");
    Json mesh = Json::object();
    mesh["primitive"] = Json::of_str("box");
    node["mesh"] = mesh;
    add["node"] = node;
    std::fprintf(stderr, "[p8] node.add\n"); Json r3 = dispatch(add);
    check(r3.find("ok")->b && r3.find("path")->str == "root/T",
          "protocol.node_add", results);
    Json cap = Json::object();
    cap["cmd"] = Json::of_str("frame.capture");
    cap["width"] = Json::of_num(320);
    cap["height"] = Json::of_num(240);
    std::fprintf(stderr, "[p8] capture\n"); Json r4 = dispatch(cap);
    // нет камеры → честная ошибка
    check(!r4.find("ok")->b, "protocol.capture_no_camera_error", results);
  }

  AF_STAGE("done");
  Json out = Json::object();
  out["ok"] = Json::of_bool(g_failed == 0);
  out["checks"] = Json::of_num(g_checks);
  out["failed"] = Json::of_num(g_failed);
  out["results"] = results;
  return out;
}

} // namespace af
