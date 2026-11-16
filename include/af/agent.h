// AgentForge — агентская сессия: JSON-RPC dispatch, плагины (C ABI),
// selftest, benchmark. Всё управление движком — через строки JSON.
#pragma once
#include "af/scene.h"
#include "af/render.h"
#include "af/assets.h"
#include "af/sim.h"
#include <memory>
#include <string>

namespace af {

constexpr const char* ENGINE_VERSION = "0.1.0";

// ── C ABI плагин игровой логики ──
struct AfApi {
  void* session;
  int   (*node_add)(void* session, const char* parent_path, const char* node_json);
  int   (*node_set)(void* session, const char* path, const char* props_json);
  const char* (*node_get)(void* session, const char* path);   // static buffer
  int   (*node_remove)(void* session, const char* path);
  double (*score_get)(void* session, const char* key);
  void  (*score_set)(void* session, const char* key, double v);
  void  (*log)(const char* msg);
  double (*time)(void* session);
  Json  (*last_events)(void* session);
};

typedef int  (*af_plugin_init_fn)(const AfApi*);
typedef void (*af_plugin_update_fn)(float dt, double time);
typedef const char* (*af_plugin_event_fn)(const char* json);

class Session {
public:
  SceneTree tree;
  Renderer renderer;
  AssetCatalog catalog;
  AssetSearchModel model;
  PhysicsParams physics;
  int viewport_w = 640, viewport_h = 360;
  std::string last_error;
  Json last_frame_report;
  // plugin
  void* plugin_handle = nullptr;
  af_plugin_init_fn p_init = nullptr;
  af_plugin_update_fn p_update = nullptr;
  af_plugin_event_fn p_event = nullptr;
  std::string get_buffer;

  Json dispatch(const Json& req);           // {"cmd": "...", ...} → ответ
  Json selftest();
  Json benchmark();
  Json schema() const;

  // AfApi-мосты (public, чтобы C-функции могли звать)
  static int api_node_add(void* s, const char* parent_path, const char* node_json);
  static int api_node_set(void* s, const char* path, const char* props_json);
  static const char* api_node_get(void* s, const char* path);
  static int api_node_remove(void* s, const char* path);
  static double api_score_get(void* s, const char* key);
  static void api_score_set(void* s, const char* key, double v);
  static void api_log(const char* msg);
  static double api_time(void* s);
  static Json api_last_events(void* s);

private:
  std::map<std::string, std::shared_ptr<Mesh>> mesh_cache_;
  Json api_event_log_;

  std::shared_ptr<Mesh> get_mesh(const Json& mesh_spec, std::string* err);
  Json capture_frame(int w, int h, const std::string& save_path);
  Json node_props_json(const NodePtr& n);
  Json node_types_schema() const;
};

} // namespace af
