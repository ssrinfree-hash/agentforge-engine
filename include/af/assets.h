// AgentForge — ассеты: OBJ+MTL, OFF; каталог ассетов; обученный семантический поиск.
// Веса (asset_model.bin) экспортирует Kaggle-ядро (ModelNet40, 12 311 моделей).
#pragma once
#include "af/scene.h"
#include <unordered_map>
#include <vector>

namespace af {

// ── Загрузчики ──
std::shared_ptr<Mesh> load_obj(const std::string& path, std::string* err = nullptr);
std::shared_ptr<Mesh> load_off(const std::string& path, std::string* err = nullptr);
std::shared_ptr<Mesh> load_any(const std::string& path, std::string* err = nullptr);

// ── Каталог ассетов (из Kaggle-ядра: catalog.jsonl) ──
struct CatalogEntry {
  std::string id;          // имя файла без расширения
  std::string cls;         // класс ассета ("chair", "lamp", ...)
  std::string name;        // читаемое имя
  uint32_t verts = 0, faces = 0;
  float bbox[6] = {0, 0, 0, 0, 0, 0};
};
class AssetCatalog {
public:
  std::vector<CatalogEntry> entries;
  static AssetCatalog load_jsonl(const std::string& path, std::string* err = nullptr);
};

// ── Обученная модель поиска: hash-ngram(text) → 40 классов (linear softmax) ──
class AssetSearchModel {
public:
  bool load(const std::string& bin_path, std::string* err = nullptr);
  bool loaded() const { return n_classes_ > 0 && !W_.empty(); }

  // Текст → скоры классов; catalog → топ-K записей. Работает без весов:
  // fallback — точное совпадение подстроки по классам.
  Json search(const std::string& query, const AssetCatalog& cat, int top_k);

private:
  int vocab_ = 4096, n_classes_ = 0, dim_ = 40;
  std::vector<float> W_;                      // [vocab x n_classes]
  std::vector<std::string> class_names_;
  std::unordered_map<std::string, int> fnv_index_;
  void hash_features(const std::string& text, std::vector<float>& feats) const;
};

// ── Встроенные примитивы (куб, плоскость, сфера — без внешних файлов) ──
std::shared_ptr<Mesh> make_box(const Vec3& size);
std::shared_ptr<Mesh> make_plane(float size);
std::shared_ptr<Mesh> make_sphere(float radius, int seg = 16);
std::shared_ptr<Mesh> make_capsule(float radius, float height);  // приближение цилиндр+полусферы

} // namespace af
