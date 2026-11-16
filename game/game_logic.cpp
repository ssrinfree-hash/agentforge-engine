// OrbRun — игровая логика тестовой игры AgentForge (плагин C ABI).
// Игрок автоматически ищет ближайшую монету, подбирает её, счёт растёт;
// победа при сборе всех монет, поражение по таймеру.
//
// Сборка: g++ -std=c++17 -O2 -fPIC -shared game_logic.cpp -o ../build/game_logic.so
#include "af/agent.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace af;

static AfApi g_api;
static std::string g_player = "Player";
static std::vector<std::string> g_coins;
static double g_speed = 8.5;
static double g_pickup = 0.9;
static double g_max_seconds = 75.0;
static char g_status_buf[1024];
static bool g_finished = false;

static bool read_pos(const char* path, Vec3& out) {
  const char* raw = g_api.node_get(g_api.session, path);
  if (!raw) return false;
  Json j;
  if (!Json::parse(raw, j)) return false;
  auto node = j.find("node");
  if (!node) return false;
  auto pos = node->find("position");
  if (!pos || pos->arr.size() < 3) return false;
  out = Vec3{(float)pos->arr[0].num, (float)pos->arr[1].num, (float)pos->arr[2].num};
  return true;
}

extern "C" int af_plugin_init(const AfApi* api) {
  g_api = *api;
  g_api.log("[orbrun] plugin init");
  return 0;
}

extern "C" void af_plugin_update(float dt, double time) {
  (void)dt;
  if (g_finished || g_player.empty()) return;
  if (g_api.score_get(g_api.session, "win") != 0.0) { g_finished = true; return; }
  if (time > g_max_seconds) {
    g_api.score_set(g_api.session, "win", -1.0);
    g_api.log("[orbrun] TIMEOUT: lose");
    g_finished = true;
    return;
  }
  Vec3 ppos;
  if (!read_pos(g_player.c_str(), ppos)) return;

  // ближайшая живая монета
  std::string best;
  Vec3 best_pos;
  float best_d = 1e9f;
  for (auto& c : g_coins) {
    Vec3 cpos;
    if (!read_pos(c.c_str(), cpos)) continue;   // уже собрана (нода удалена)
    float dx = cpos.x - ppos.x, dz = cpos.z - ppos.z;
    float d = std::sqrt(dx * dx + dz * dz);
    if (d < best_d) { best_d = d; best = c; best_pos = cpos; }
  }
  if (best.empty()) {
    g_api.score_set(g_api.session, "win", 1.0);
    g_api.log("[orbrun] WIN: все монеты собраны");
    g_finished = true;
    return;
  }
  // движение к монете (горизонталь)
  float dx = best_pos.x - ppos.x, dz = best_pos.z - ppos.z;
  float len = std::sqrt(dx * dx + dz * dz) + 1e-5f;
  float vx = dx / len * (float)g_speed;
  float vz = dz / len * (float)g_speed;
  Json props = Json::object();
  Json vel = Json::array();
  vel.push(Json::of_num(vx)); vel.push(Json::of_num(0)); vel.push(Json::of_num(vz));
  props["velocity"] = vel;
  Json posj = Json::array();
  posj.push(Json::of_num(ppos.x + vx * (float)dt * 0.5f));
  posj.push(Json::of_num(ppos.y));
  posj.push(Json::of_num(ppos.z + vz * (float)dt * 0.5f));
  props["position"] = posj;
  g_api.node_set(g_api.session, g_player.c_str(), props.dump().c_str());

  if (best_d < g_pickup) {
    g_api.node_remove(g_api.session, best.c_str());
    double score = g_api.score_get(g_api.session, "score") + 1.0;
    g_api.score_set(g_api.session, "score", score);
    std::string msg = "[orbrun] pickup " + best + " score=" + std::to_string((int)score);
    g_api.log(msg.c_str());
  }
}

extern "C" const char* af_plugin_event(const char* json) {
  Json ev;
  if (!Json::parse(json, ev)) return "{\"ok\":false}";
  auto op = ev.find("op");
  if (op && op->str == "setup") {
    if (auto p = ev.find("player")) g_player = p->str;
    g_coins.clear();
    if (auto cs = ev.find("coins"); cs && cs->is_arr())
      for (auto& c : cs->arr) g_coins.push_back(c.str);
    g_speed = ev.find("speed") ? ev.find("speed")->as_num(8.5) : 8.5;
    g_pickup = ev.find("pickup_dist") ? ev.find("pickup_dist")->as_num(0.9) : 0.9;
    g_max_seconds = ev.find("max_seconds") ? ev.find("max_seconds")->as_num(75) : 75;
    g_api.log("[orbrun] setup done");
    std::snprintf(g_status_buf, sizeof g_status_buf,
                  "{\"ok\":true,\"op\":\"setup\",\"coins\":%zu}", g_coins.size());
    return g_status_buf;
  }
  if (op && op->str == "status") {
    int alive = 0;
    for (auto& c : g_coins) {
      Vec3 tmp;
      if (read_pos(c.c_str(), tmp)) ++alive;
    }
    std::snprintf(g_status_buf, sizeof g_status_buf,
                  "{\"ok\":true,\"op\":\"status\",\"score\":%d,\"coins_left\":%d,"
                  "\"finished\":%s,\"time\":%.2f}",
                  (int)g_api.score_get(g_api.session, "score"), alive,
                  g_finished ? "true" : "false", g_api.time(g_api.session));
    return g_status_buf;
  }
  return "{\"ok\":false,\"error\":\"unknown op\"}";
}
