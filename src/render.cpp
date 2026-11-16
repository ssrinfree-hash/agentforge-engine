// AgentForge — реализация рендера.
#include "af/render.h"
#include <chrono>
#include <cstdio>
#include <cstring>

namespace af {

static uint64_t fnv1a(const uint8_t* data, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 1099511628211ull; }
  return h;
}

bool Frame::save_ppm(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; ++i)
    std::fwrite(&color[i * 4], 1, 3, f);
  std::fclose(f);
  return true;
}

bool Frame::save_raw(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  int32_t magic = 0x41465246;  // "AFRF"
  std::fwrite(&magic, 4, 1, f);
  std::fwrite(&width, 4, 1, f);
  std::fwrite(&height, 4, 1, f);
  std::fwrite(color.data(), 1, color.size(), f);
  std::fwrite(depth.data(), 4, depth.size(), f);
  std::fclose(f);
  return true;
}

void Renderer::set_viewport(int w, int h) {
  w_ = w; h_ = h;
  color_.assign(size_t(w) * h * 4, 0);
  depth_.assign(size_t(w) * h, 1.0f);
}

void Renderer::draw_mesh(const Mesh& mesh, const Mat4& world, const Mat4& vp,
                         const Vec3& tint, const Vec3& light_dir,
                         const Vec3& light_color) {
  if (!mesh.valid()) return;
  const size_t nv = mesh.pos.size();
  thread_local std::vector<Vec4> clip;
  clip.assign(nv, Vec4{});
  for (size_t i = 0; i < nv; ++i) {
    Vec3 wp = world.transform_point(mesh.pos[i]);
    clip[i] = vp.transform_point4(wp);       // полный clip-space (w корректный)
  }
  for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
    uint32_t i0 = mesh.idx[t], i1 = mesh.idx[t + 1], i2 = mesh.idx[t + 2];
    if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
    Vec4 c0 = clip[i0], c1 = clip[i1], c2 = clip[i2];
    float w0 = c0.w, w1 = c1.w, w2 = c2.w;
    if (w0 < 1e-6f || w1 < 1e-6f || w2 < 1e-6f) continue;   // near-clip упрощённый
    float x0 = c0.x / w0, y0 = c0.y / w0, z0 = c0.z / w0;   // NDC
    float x1 = c1.x / w1, y1 = c1.y / w1, z1 = c1.z / w1;
    float x2 = c2.x / w2, y2 = c2.y / w2, z2 = c2.z / w2;
    // экранные координаты
    float sx0 = (x0 * 0.5f + 0.5f) * w_, sy0 = (1 - (y0 * 0.5f + 0.5f)) * h_;
    float sx1 = (x1 * 0.5f + 0.5f) * w_, sy1 = (1 - (y1 * 0.5f + 0.5f)) * h_;
    float sx2 = (x2 * 0.5f + 0.5f) * w_, sy2 = (1 - (y2 * 0.5f + 0.5f)) * h_;
    // backface culling в экранных координатах
    float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (area <= 0) continue;
    float area_inv = 1.0f / area;
    // Lambert-освещение по мировой нормали
    Vec3 n0 = world.transform_dir(mesh.nrm[i0]).normalized();
    Vec3 n1 = world.transform_dir(mesh.nrm[i1]).normalized();
    Vec3 n2 = world.transform_dir(mesh.nrm[i2]).normalized();
    Vec3 base = mesh.color * tint;
    auto shade = [&](const Vec3& n) {
      float d = std::max(0.0f, -n.dot(light_dir));
      Vec3 c{base.x * (0.35f + 0.75f * d) * light_color.x,
             base.y * (0.35f + 0.75f * d) * light_color.y,
             base.z * (0.35f + 0.75f * d) * light_color.z};
      return c;
    };
    Vec3 col[3] = {shade(n0), shade(n1), shade(n2)};
    // барицентрический растер с z-интерполяцией (персп. корректность упрощена)
    int minx = std::max(0, (int)std::floor(std::min({sx0, sx1, sx2})));
    int maxx = std::min(w_ - 1, (int)std::ceil(std::max({sx0, sx1, sx2})));
    int miny = std::max(0, (int)std::floor(std::min({sy0, sy1, sy2})));
    int maxy = std::min(h_ - 1, (int)std::ceil(std::max({sy0, sy1, sy2})));
    if (minx > maxx || miny > maxy) continue;
    ++drawn_tris_;
    for (int y = miny; y <= maxy; ++y) {
      for (int x = minx; x <= maxx; ++x) {
        float px = x + 0.5f, py = y + 0.5f;
        float l0 = ((sx1 - sx0) * (py - sy0) - (sy1 - sy0) * (px - sx0)) * area_inv;
        float l1 = ((px - sx0) * (sy2 - sy0) - (py - sy0) * (sx2 - sx0)) * area_inv;
        float l2 = 1.0f - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        // NDC-z аффинно в экранных барицентриках → линейная интерполяция корректна
        float z_ndc = l0 * z0 + l1 * z1 + l2 * z2;
        float depth01 = std::clamp((z_ndc + 1.0f) * 0.5f, 0.0f, 1.0f);
        size_t di = size_t(y) * w_ + x;
        if (depth01 < depth_[di]) {
          depth_[di] = depth01;
          Vec3 c = col[0] * l0 + col[1] * l1 + col[2] * l2;
          color_[di * 4 + 0] = (uint8_t)(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
          color_[di * 4 + 1] = (uint8_t)(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
          color_[di * 4 + 2] = (uint8_t)(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
          color_[di * 4 + 3] = 255;
        }
      }
    }
  }
}

Frame Renderer::render(const SceneTree& tree, const std::shared_ptr<Camera3D>& cam,
                       const Vec3& light_dir, const Vec3& light_color,
                       float light_intensity) {
  auto t0 = std::chrono::steady_clock::now();
  color_.assign(size_t(w_) * h_ * 4, 0);
  depth_.assign(size_t(w_) * h_, 1.0f);
  for (size_t i = 0; i < color_.size(); i += 4) {
    color_[i] = (uint8_t)(clear_color.x * 255);
    color_[i + 1] = (uint8_t)(clear_color.y * 255);
    color_[i + 2] = (uint8_t)(clear_color.z * 255);
    color_[i + 3] = 255;
  }
  drawn_tris_ = 0;
  Mat4 view = cam->view();
  Mat4 proj = cam->projection(float(w_) / float(h_));
  Mat4 vp = proj * view;
  Vec3 ldir = light_dir.normalized();
  Vec3 lcol = light_color * light_intensity;
  for (auto& mi : tree.mesh_instances()) {
    if (!mi->visible || !mi->mesh || !mi->enabled) continue;
    draw_mesh(*mi->mesh, mi->world_matrix(), vp, mi->color, ldir, lcol);
  }
  Frame f;
  f.width = w_; f.height = h_;
  f.color = std::move(color_);
  f.depth = std::move(depth_);
  f.drawn_tris = drawn_tris_;
  f.pixel_hash = fnv1a(f.color.data(), f.color.size());
  auto t1 = std::chrono::steady_clock::now();
  f.render_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  color_.clear(); depth_.clear();
  return f;
}

FrameReport Renderer::report(const SceneTree& tree, const Frame& frame,
                             const std::shared_ptr<Camera3D>& cam) {
  FrameReport r;
  r.frame = tree.frame;
  r.sim_time = tree.time;
  r.width = frame.width;
  r.height = frame.height;
  r.cam_pos = cam->world_matrix().transform_point(Vec3{});
  auto wq = cam->world_matrix();
  r.cam_rot = cam->rotation;
  (void)wq;
  r.fov_deg = cam->fov_deg;
  r.drawn_tris = frame.drawn_tris;
  r.render_ms = frame.render_ms;
  r.pixel_hash = frame.pixel_hash;
  float aspect = float(frame.width) / float(frame.height);
  Mat4 view = cam->view();
  Mat4 vp = cam->projection(aspect) * view;
  for (auto& mi : tree.mesh_instances()) {
    if (!mi->visible || !mi->enabled) continue;
    FrameReport::Obj o;
    o.name = mi->name;
    o.path = mi->path();
    o.type = mi->type;
    o.mesh_class = mi->mesh_class;
    o.world_pos = mi->world_matrix().transform_point(Vec3{});
    o.color = mi->color;
    o.tris = mi->mesh ? uint32_t(mi->mesh->idx.size() / 3) : 0;
    AABB b = MeshInstance3D_world_aabb(*mi);
    if (!b.empty) {
      float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
      float dmin = 1e9f, dmax = -1e9f;
      Vec3 corners[8] = {{b.mn.x,b.mn.y,b.mn.z},{b.mx.x,b.mn.y,b.mn.z},
                         {b.mn.x,b.mx.y,b.mn.z},{b.mx.x,b.mx.y,b.mn.z},
                         {b.mn.x,b.mn.y,b.mx.z},{b.mx.x,b.mn.y,b.mx.z},
                         {b.mn.x,b.mx.y,b.mx.z},{b.mx.x,b.mx.y,b.mx.z}};
      int inside = 0;
      for (auto& c : corners) {
        Vec4 cp4 = vp.transform_point4(c);
        if (cp4.w > 1e-6f) {      // перед камерой
          ++inside;
          float ndc_x = cp4.x / cp4.w, ndc_y = cp4.y / cp4.w;
          float sx = (ndc_x * 0.5f + 0.5f) * frame.width;
          float sy = (1 - (ndc_y * 0.5f + 0.5f)) * frame.height;
          minx = std::min(minx, sx); maxx = std::max(maxx, sx);
          miny = std::min(miny, sy); maxy = std::max(maxy, sy);
          dmin = std::min(dmin, cp4.w); dmax = std::max(dmax, cp4.w);
        }
      }
      if (inside > 0) {
        o.visible = true;
        o.screen_bbox[0] = minx; o.screen_bbox[1] = miny;
        o.screen_bbox[2] = maxx; o.screen_bbox[3] = maxy;
        o.depth_min = dmin; o.depth_max = dmax;
      }
    }
    r.objects.push_back(std::move(o));
  }
  return r;
}

Json FrameReport::to_json(const SceneTree& tree) const {
  Json j = Json::object();
  j["frame"] = Json::of_num(frame);
  j["sim_time"] = Json::of_num(sim_time);
  Json res = Json::array();
  res.push(Json::of_num(width)); res.push(Json::of_num(height));
  j["resolution"] = res;
  Json cam = Json::object();
  Json cp = Json::array();
  cp.push(Json::of_num(cam_pos.x)); cp.push(Json::of_num(cam_pos.y)); cp.push(Json::of_num(cam_pos.z));
  cam["position"] = cp;
  cam["fov_deg"] = Json::of_num(fov_deg);
  j["camera"] = cam;
  Json objs = Json::array();
  for (auto& o : objects) {
    Json jo = Json::object();
    jo["name"] = Json::of_str(o.name);
    jo["path"] = Json::of_str(o.path);
    jo["type"] = Json::of_str(o.type);
    jo["class"] = Json::of_str(o.mesh_class);
    Json bb = Json::array();
    bb.push(Json::of_num(o.screen_bbox[0])); bb.push(Json::of_num(o.screen_bbox[1]));
    bb.push(Json::of_num(o.screen_bbox[2])); bb.push(Json::of_num(o.screen_bbox[3]));
    jo["screen_bbox"] = bb;
    Json d = Json::array();
    d.push(Json::of_num(o.depth_min)); d.push(Json::of_num(o.depth_max));
    jo["depth_range"] = d;
    jo["visible"] = Json::of_bool(o.visible);
    Json wp = Json::array();
    wp.push(Json::of_num(o.world_pos.x)); wp.push(Json::of_num(o.world_pos.y)); wp.push(Json::of_num(o.world_pos.z));
    jo["world_pos"] = wp;
    Json cc = Json::array();
    cc.push(Json::of_num(o.color.x)); cc.push(Json::of_num(o.color.y)); cc.push(Json::of_num(o.color.z));
    jo["color"] = cc;
    jo["tris"] = Json::of_num(o.tris);
    objs.push(jo);
  }
  j["objects"] = objs;
  Json st = Json::object();
  st["drawn_tris"] = Json::of_num(drawn_tris);
  st["render_ms"] = Json::of_num(render_ms);
  st["pixel_hash"] = Json::of_num(double(pixel_hash));
  j["stats"] = st;
  Json sc = Json::object();
  for (auto& [k, v] : tree.scores) sc[k] = Json::of_num(v);
  j["scores"] = sc;
  return j;
}

} // namespace af
