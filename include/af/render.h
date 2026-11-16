// AgentForge — рендер: offscreen software-растеризатор + FrameReport.
// «Экран для ИИ»: кадр = пиксельный буфер (PPM/RAW) + машиночитаемый отчёт
// (объекты, экранные bbox, глубина, видимость) — человек ничего не открывает.
#pragma once
#include "af/scene.h"
#include <cstdint>
#include <string>
#include <vector>

namespace af {

struct Frame {
  int width = 0, height = 0;
  std::vector<uint8_t> color;        // RGBA8, width*height*4
  std::vector<float> depth;          // [0,1] нормированная глубина
  uint64_t pixel_hash = 0;           // FNV-1a — быстрый «отпечаток» кадра
  uint32_t drawn_tris = 0;
  double render_ms = 0;

  bool save_ppm(const std::string& path) const;            // кадр целиком (кросс-платформенно)
  bool save_raw(const std::string& path) const;            // color+depth бинарь (для пайплайнов)
};

struct FrameReport {                     // машиночитаемый «скриншот»
  Json to_json(const SceneTree& tree) const;
  int frame = 0;
  double sim_time = 0;
  int width = 0, height = 0;
  Vec3 cam_pos; Quat cam_rot; float fov_deg = 0;
  struct Obj {
    std::string name, path, type, mesh_class;
    float screen_bbox[4] = {0, 0, 0, 0};   // x0,y0,x1,y1 (пиксели)
    float depth_min = 0, depth_max = 0;
    bool visible = false;
    Vec3 world_pos;
    Vec3 color;
    uint32_t tris = 0;
  };
  std::vector<Obj> objects;
  uint32_t drawn_tris = 0;
  double render_ms = 0;
  uint64_t pixel_hash = 0;
};

class Renderer {
public:
  void set_viewport(int w, int h);
  Vec3 clear_color{0.10f, 0.12f, 0.16f};
  bool wireframe = false;

  Frame render(const SceneTree& tree, const std::shared_ptr<Camera3D>& cam,
               const Vec3& light_dir, const Vec3& light_color, float light_intensity);

  // Отчёт по объектам (независимо от окклюзии — bbox через проекцию bbox ноды)
  static FrameReport report(const SceneTree& tree, const Frame& frame,
                            const std::shared_ptr<Camera3D>& cam);

private:
  int w_ = 0, h_ = 0;
  std::vector<uint8_t> color_;
  std::vector<float> depth_;
  uint32_t drawn_tris_ = 0;

  void draw_mesh(const Mesh& mesh, const Mat4& world, const Mat4& vp,
                 const Vec3& tint, const Vec3& light_dir, const Vec3& light_color);
};

} // namespace af
