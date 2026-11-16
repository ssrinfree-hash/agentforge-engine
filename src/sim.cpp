// AgentForge — реализация физики.
#include "af/sim.h"
#include <cmath>

namespace af {

Json physics_step(SceneTree& tree, float dt, const PhysicsParams& pp) {
  dt = std::min(dt, pp.max_dt);
  Json events = Json::array();
  auto dynamics = tree.mesh_instances();
  // соберём статические AABB (кэшируется на шаг — сцен небольшие)
  struct Static { std::string path; AABB box; };
  std::vector<Static> statics;
  for (auto& mi : dynamics)
    if (mi->static_body && !mi->body) {
      AABB b = MeshInstance3D_world_aabb(*mi);
      if (!b.empty) statics.push_back({mi->path(), b});
    }
  for (auto& mi : dynamics) {
    if (!mi->body || mi->static_body || !mi->enabled) continue;
    Vec3 prev_vel = mi->velocity;
    mi->velocity.y += pp.gravity * dt;
    Vec3 pos = mi->world_matrix().transform_point(Vec3{});
    Vec3 new_pos = pos + mi->velocity * dt;

    // AABB тела (по локальным габаритам меша)
    AABB local = mi->mesh ? mi->mesh->bounds : AABB(Vec3{-0.5f,-0.5f,-0.5f}, Vec3{0.5f,0.5f,0.5f});
    Vec3 he = local.empty ? Vec3{0.5f,0.5f,0.5f} : local.size() * 0.5f;

    // земля
    if (new_pos.y - he.y < pp.ground_y) {
      new_pos.y = pp.ground_y + he.y;
      if (mi->velocity.y < 0) {
        mi->velocity.y = -mi->velocity.y * pp.restitution;
        if (std::fabs(mi->velocity.y) < 0.6f) mi->velocity.y = 0;
        if (std::fabs(prev_vel.y) > 1.0f) {           // событие только при заметном ударе
          Json ev = Json::object();
          ev["event"] = Json::of_str("ground_hit");
          ev["node"] = Json::of_str(mi->path());
          ev["impact"] = Json::of_num(std::fabs(prev_vel.y));
          events.push(ev);
        }
      }
      mi->velocity.x *= pp.friction;
      mi->velocity.z *= pp.friction;
    }
    // статические коллизии (простое разрешение по минимальному проникновению)
    AABB candidate(Vec3{new_pos.x-he.x,new_pos.y-he.y,new_pos.z-he.z},
                   Vec3{new_pos.x+he.x,new_pos.y+he.y,new_pos.z+he.z});
    for (auto& st : statics) {
      if (!candidate.intersects(st.box)) continue;
      // проникновения по осям
      float dx1 = st.box.mx.x - candidate.mn.x, dx2 = candidate.mx.x - st.box.mn.x;
      float dy1 = st.box.mx.y - candidate.mn.y, dy2 = candidate.mx.y - st.box.mn.y;
      float dz1 = st.box.mx.z - candidate.mn.z, dz2 = candidate.mx.z - st.box.mn.z;
      float px = std::min(dx1, dx2), py = std::min(dy1, dy2), pz = std::min(dz1, dz2);
      if (py <= px && py <= pz) {
        new_pos.y += dy1 < dy2 ? py : -py;
        mi->velocity.y = dy1 < dy2 ? std::max(mi->velocity.y, 0.0f)
                                   : std::min(mi->velocity.y, 0.0f) * -pp.restitution;
      } else if (px <= pz) {
        new_pos.x += dx1 < dx2 ? px : -px;
        mi->velocity.x = 0;
      } else {
        new_pos.z += dz1 < dz2 ? pz : -pz;
        mi->velocity.z = 0;
      }
      candidate = AABB(Vec3{new_pos.x-he.x,new_pos.y-he.y,new_pos.z-he.z},
                       Vec3{new_pos.x+he.x,new_pos.y+he.y,new_pos.z+he.z});
      Json ev = Json::object();
      ev["event"] = Json::of_str("collision");
      ev["node"] = Json::of_str(mi->path());
      ev["with"] = Json::of_str(st.path);
      events.push(ev);
    }
    mi->position = new_pos;
    mi->invalidate();
  }
  Json out = Json::object();
  out["events"] = events;
  out["dt"] = Json::of_num(dt);
  return out;
}

Json raycast(const SceneTree& tree, const Vec3& origin, const Vec3& dir, float max_dist) {
  Json out = Json::object();
  Vec3 d = dir.normalized();
  float best = max_dist;
  std::string best_path;
  for (auto& mi : tree.mesh_instances()) {
    if (!mi->visible || !mi->enabled) continue;
    AABB b = MeshInstance3D_world_aabb(*mi);
    if (b.empty) continue;
    // slab-метод
    float t0 = 0.0f, t1 = best;
    bool hit = true;
    float axes[3] = {d.x, d.y, d.z};
    float o[3] = {origin.x, origin.y, origin.z};
    float mn[3] = {b.mn.x, b.mn.y, b.mn.z}, mx[3] = {b.mx.x, b.mx.y, b.mx.z};
    for (int a = 0; a < 3; ++a) {
      if (std::fabs(axes[a]) < 1e-9f) {
        if (o[a] < mn[a] || o[a] > mx[a]) { hit = false; break; }
        continue;
      }
      float ta = (mn[a] - o[a]) / axes[a], tb = (mx[a] - o[a]) / axes[a];
      if (ta > tb) std::swap(ta, tb);
      t0 = std::max(t0, ta);
      t1 = std::min(t1, tb);
      if (t0 > t1) { hit = false; break; }
    }
    if (hit && t0 < best) { best = t0; best_path = mi->path(); }
  }
  out["hit"] = Json::of_bool(!best_path.empty());
  if (!best_path.empty()) {
    out["path"] = Json::of_str(best_path);
    out["dist"] = Json::of_num(best);
    Vec3 p = origin + d * best;
    Json pj = Json::array();
    pj.push(Json::of_num(p.x)); pj.push(Json::of_num(p.y)); pj.push(Json::of_num(p.z));
    out["point"] = pj;
  }
  return out;
}

} // namespace af
