// AgentForge — точка входа: serve | run | selftest | schema | benchmark | render
#include "af/agent.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace af;

static void load_data(Session& s, const std::string& catalog, const std::string& weights,
                      const std::string& scene_path) {
  if (!catalog.empty()) {
    std::string e;
    s.catalog = AssetCatalog::load_jsonl(catalog, &e);
    if (!e.empty()) std::fprintf(stderr, "catalog: %s\n", e.c_str());
    else std::fprintf(stderr, "catalog: %zu entries\n", s.catalog.entries.size());
  }
  if (!weights.empty()) {
    std::string e;
    if (s.model.load(weights, &e)) std::fprintf(stderr, "asset model: loaded\n");
    else std::fprintf(stderr, "asset model: %s\n", e.c_str());
  }
  if (!scene_path.empty()) {
    std::ifstream f(scene_path);
    std::stringstream ss; ss << f.rdbuf();
    Json j;
    std::string err;
    if (Json::parse(ss.str(), j, &err) && s.tree.from_json(j, &err))
      std::fprintf(stderr, "scene: %s loaded\n", scene_path.c_str());
    else std::fprintf(stderr, "scene error: %s\n", err.c_str());
  }
}

static Json run_selftest_report(Session& s) {
  Json r = s.selftest();
  std::printf("%s\n", r.dump(2).c_str());
  return r;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
        "AgentForge %s — игровой движок для ИИ-агентов\n"
        "Использование:\n"
        "  afengine serve [--scene S] [--catalog C] [--weights W]\n"
        "      JSON-RPC по stdin/stdout (по строке на запрос)\n"
        "  afengine run --scene S --frames N [--dt 0.016] [--report out.json] [--capture-dir D]\n"
        "      прогон симуляции headless + FrameReport\n"
        "  afengine render --scene S [--width 640] [--height 360] [--out frame.ppm]\n"
        "  afengine selftest [--catalog C] [--weights W]\n"
        "  afengine schema\n"
        "  afengine benchmark [--catalog C] [--weights W]\n",
        ENGINE_VERSION);
    return 2;
  }
  std::string mode = argv[1];
  std::string scene_path, catalog, weights, report_path, capture_dir, out_path;
  int frames = 300, width = 640, height = 360;
  float dt = 1.0f / 60.0f;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--scene") scene_path = v;
    else if (k == "--catalog") catalog = v;
    else if (k == "--weights") weights = v;
    else if (k == "--frames") frames = std::atoi(v.c_str());
    else if (k == "--dt") dt = (float)std::atof(v.c_str());
    else if (k == "--report") report_path = v;
    else if (k == "--capture-dir") capture_dir = v;
    else if (k == "--out") out_path = v;
    else if (k == "--width") width = std::atoi(v.c_str());
    else if (k == "--height") height = std::atoi(v.c_str());
  }

  Session session;
  load_data(session, catalog, weights, mode != "serve" ? scene_path : "");
  if (mode == "serve") load_data(session, catalog, weights, scene_path);

  if (mode == "schema") {
    std::printf("%s\n", session.schema().dump(2));
    return 0;
  }
  if (mode == "selftest") {
    Json r = run_selftest_report(session);
    return r.find("ok")->b ? 0 : 1;
  }
  if (mode == "benchmark") {
    std::printf("%s\n", session.benchmark().dump(2).c_str());
    return 0;
  }
  if (mode == "render") {
    Json req = Json::object();
    req["cmd"] = Json::of_str("frame.capture");
    req["width"] = Json::of_num(width);
    req["height"] = Json::of_num(height);
    req["save"] = Json::of_str(out_path.empty() ? "/tmp/af_frame.ppm" : out_path);
    Json r = session.dispatch(req);
    std::printf("%s\n", r.dump(2).c_str());
    return r.find("ok")->b ? 0 : 1;
  }
  if (mode == "run") {
    if (scene_path.empty()) { std::fprintf(stderr, "run: --scene required\n"); return 2; }
    Json events_all = Json::array();
    for (int i = 0; i < frames; ++i) {
      Json req = Json::object();
      req["cmd"] = Json::of_str("sim.step");
      req["frames"] = Json::of_num(1);
      req["dt"] = Json::of_num(dt);
      Json r = session.dispatch(req);
      if (auto ev = r.find("events"))
        for (auto& e : ev->arr) events_all.push(e);
      if (!capture_dir.empty() && i % 30 == 0) {
        Json cap = Json::object();
        cap["cmd"] = Json::of_str("frame.capture");
        cap["width"] = Json::of_num(width);
        cap["height"] = Json::of_num(height);
        cap["save"] = Json::of_str(capture_dir + "/frame_" + std::to_string(session.tree.frame) + ".ppm");
        session.dispatch(cap);
      }
    }
    Json out = Json::object();
    out["engine"] = Json::of_str(std::string("AgentForge ") + ENGINE_VERSION);
    out["frames_run"] = Json::of_num(frames);
    if (session.last_frame_report.is_null()) {
      Json cap = Json::object();
      cap["cmd"] = Json::of_str("frame.capture");
      cap["width"] = Json::of_num(width);
      cap["height"] = Json::of_num(height);
      session.dispatch(cap);
    }
    out["final_frame"] = session.last_frame_report;
    out["events"] = events_all;
    Json sc = Json::object();
    for (auto& [k, v] : session.tree.scores) sc[k] = Json::of_num(v);
    out["scores"] = sc;
    std::string text = out.dump(2);
    if (!report_path.empty()) {
      std::ofstream f(report_path);
      f << text;
    }
    std::printf("%s\n", text.c_str());
    return 0;
  }
  if (mode == "serve") {
    std::string line;
    std::ios::sync_with_stdio(false);
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      Json req;
      std::string err;
      Json resp;
      if (!Json::parse(line, req, &err)) {
        resp = Json::object();
        resp["ok"] = Json::of_bool(false);
        resp["error"] = Json::of_str("bad request json: " + err);
      } else {
        resp = session.dispatch(req);
      }
      std::cout << resp.dump() << "\n" << std::flush;
      if (auto b = resp.find("bye"); b && b->b) break;
    }
    return 0;
  }
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
