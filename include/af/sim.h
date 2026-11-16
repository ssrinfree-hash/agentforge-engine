// AgentForge — симуляция: физика поверх SceneTree.
#pragma once
#include "af/scene.h"
#include "af/json.h"

namespace af {

struct PhysicsParams {
  float gravity = -9.8f;         // м/с²
  float ground_y = 0.0f;         // плоскость земли y=0
  float restitution = 0.35f;     // упругость отскока
  float friction = 0.86f;        // затухание по земле
  float max_dt = 0.05f;
};

// Один шаг физики: интеграция динамических тел (MeshInstance3D::body=true),
// коллизии AABB против статических тел и земли. Возвращает события.
Json physics_step(SceneTree& tree, float dt, const PhysicsParams& pp);

// Raycast из точки в направлении: ближайшее пересечение с AABB нод.
// Возвращает {hit:bool, path, point, dist, normal} или hit:false.
Json raycast(const SceneTree& tree, const Vec3& origin, const Vec3& dir, float max_dist);

} // namespace af
