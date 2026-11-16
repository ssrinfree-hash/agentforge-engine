// AgentForge — реализация ассетов.
#include "af/assets.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace af {

static std::string basename_no_ext(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  size_t d = p.find_last_of('.');
  std::string b = (s == std::string::npos) ? p : p.substr(s + 1);
  return (d != std::string::npos && d > s) ? b.substr(0, d - s - 1) : b;
}

// ── OBJ + MTL ──
std::shared_ptr<Mesh> load_obj(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
  auto mesh = std::make_shared<Mesh>();
  mesh->name = basename_no_ext(path);
  std::vector<Vec3> v, vn;
  std::vector<Vec2> vt;
  std::string line;
  std::unordered_map<std::string, uint32_t> vmap;
  std::string mtl_file;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 2) continue;
    if (line.compare(0, 2, "v ") == 0) {
      float a, b, c; std::sscanf(line.c_str() + 2, "%f %f %f", &a, &b, &c);
      v.push_back({a, b, c});
    } else if (line.compare(0, 3, "vn ") == 0) {
      float a, b, c; std::sscanf(line.c_str() + 3, "%f %f %f", &a, &b, &c);
      vn.push_back({a, b, c});
    } else if (line.compare(0, 3, "vt ") == 0) {
      float a, b; std::sscanf(line.c_str() + 3, "%f %f", &a, &b);
      vt.push_back({a, b});
    } else if (line.compare(0, 7, "mtllib ") == 0) {
      mtl_file = line.substr(7);
    } else if (line.compare(0, 2, "f ") == 0) {
      // f v/vt/vn v/vt/vn v/vt/vn [v/vt/vn] — фан в треугольники
      std::istringstream ss(line.substr(2));
      std::vector<uint32_t> fv, fn, ft;
      std::string tok;
      while (ss >> tok) {
        int vi = 0, ti = 0, ni = 0;
        if (std::sscanf(tok.c_str(), "%d/%d/%d", &vi, &ti, &ni) >= 1) {
          fv.push_back(vi);
          ft.push_back(ti);
          fn.push_back(ni);
        }
      }
      auto idx_of = [&](int vi, int ti, int ni) -> uint32_t {
        std::string key = std::to_string(vi) + "/" + std::to_string(ti) + "/" + std::to_string(ni);
        auto it = vmap.find(key);
        if (it != vmap.end()) return it->second;
        uint32_t id = (uint32_t)mesh->pos.size();
        mesh->pos.push_back(v[vi - 1]);
        mesh->nrm.push_back(ni > 0 && ni <= (int)vn.size() ? vn[ni - 1] : Vec3{0, 1, 0});
        mesh->uv.push_back(ti > 0 && ti <= (int)vt.size() ? vt[ti - 1] : Vec2{});
        vmap[key] = id;
        return id;
      };
      for (size_t i = 1; i + 1 < fv.size(); ++i) {
        mesh->idx.push_back(idx_of(fv[0], ft[0], fn[0]));
        mesh->idx.push_back(idx_of(fv[i], ft[i], fn[i]));
        mesh->idx.push_back(idx_of(fv[i + 1], ft[i + 1], fn[i + 1]));
      }
    }
  }
  // MTL: только Kd первого материала (достаточно для демо)
  if (!mtl_file.empty()) {
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    std::ifstream mf(dir + mtl_file);
    std::string ml;
    while (mf && std::getline(mf, ml)) {
      if (ml.compare(0, 3, "Kd ") == 0) {
        float r, g, b; std::sscanf(ml.c_str() + 3, "%f %f %f", &r, &g, &b);
        mesh->color = {r, g, b};
        break;
      }
    }
  }
  mesh->recompute_bounds();
  if (mesh->pos.empty()) { if (err) *err = "no vertices in " + path; return nullptr; }
  return mesh;
}

// ── OFF (ModelNet40) ──
std::shared_ptr<Mesh> load_off(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) { if (err) *err = "cannot open " + path; return nullptr; }
  auto mesh = std::make_shared<Mesh>();
  mesh->name = basename_no_ext(path);
  std::string header;
  f >> header;
  if (header != "OFF" && header != "COFF") { if (err) *err = "not OFF: " + path; return nullptr; }
  int nv = 0, nf = 0, ne = 0;
  f >> nv >> nf >> ne;
  std::vector<Vec3> v(nv);
  for (int i = 0; i < nv; ++i) {
    float a, b, c; f >> a >> b >> c;
    v[i] = {a, b, c};
  }
  // нормали из граней (у OFF их нет)
  mesh->pos.reserve(nv);
  mesh->nrm.assign(nv, Vec3{});
  for (int t = 0; t < nf; ++t) {
    int cnt; f >> cnt;
    std::vector<uint32_t> ids(cnt);
    for (int k = 0; k < cnt; ++k) { int vi; f >> vi; ids[k] = (uint32_t)vi; }
    for (size_t i = 1; i + 1 < ids.size(); ++i) {
      uint32_t a = ids[0], b = ids[i], c = ids[i + 1];
      if (a >= (uint32_t)nv || b >= (uint32_t)nv || c >= (uint32_t)nv) continue;
      Vec3 n = (v[b] - v[a]).cross(v[c] - v[a]);
      mesh->idx.push_back(a); mesh->idx.push_back(b); mesh->idx.push_back(c);
      mesh->nrm[a] += n; mesh->nrm[b] += n; mesh->nrm[c] += n;
    }
  }
  for (auto& n : mesh->nrm) {
    n = n.normalized();
    if (n.len() < 1e-6f) n = Vec3{0, 1, 0};
  }
  mesh->pos = v;
  mesh->recompute_bounds();
  if (mesh->pos.empty()) { if (err) *err = "no vertices in " + path; return nullptr; }
  return mesh;
}

std::shared_ptr<Mesh> load_any(const std::string& path, std::string* err) {
  if (path.size() > 4) {
    std::string ext = path.substr(path.size() - 4);
    for (auto& c : ext) c = (char)std::tolower(c);
    if (ext == ".off") return load_off(path, err);
    if (ext == ".obj") return load_obj(path, err);
  }
  return load_obj(path, err);
}

// ── Каталог ──
AssetCatalog AssetCatalog::load_jsonl(const std::string& path, std::string* err) {
  AssetCatalog cat;
  std::ifstream f(path);
  if (!f) { if (err) *err = "cannot open " + path; return cat; }
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    Json j;
    if (!Json::parse(line, j) || !j.is_obj()) continue;
    CatalogEntry e;
    if (auto v = j.find("id")) e.id = v->as_str();
    if (auto v = j.find("class")) e.cls = v->as_str();
    if (auto v = j.find("name")) e.name = v->as_str();
    if (auto v = j.find("verts")) e.verts = (uint32_t)v->as_num(0);
    if (auto v = j.find("faces")) e.faces = (uint32_t)v->as_num(0);
    if (auto v = j.find("bbox"); v && v->is_arr() && v->arr.size() >= 6)
      for (int i = 0; i < 6; ++i) e.bbox[i] = (float)v->arr[i].num;
    if (!e.id.empty()) cat.entries.push_back(std::move(e));
  }
  return cat;
}

// ── Модель поиска ──
bool AssetSearchModel::load(const std::string& bin_path, std::string* err) {
  std::ifstream f(bin_path, std::ios::binary);
  if (!f) { if (err) *err = "cannot open " + bin_path; return false; }
  char magic[8];
  f.read(magic, 8);
  if (std::memcmp(magic, "AFASSET1", 8) != 0) { if (err) *err = "bad magic"; return false; }
  int32_t header[4] = {0, 0, 0, 0};
  f.read((char*)header, 16);                    // vocab, classes, dim, n_names
  vocab_ = header[0]; n_classes_ = header[1]; dim_ = header[2];
  if (vocab_ <= 0 || vocab_ > 100000 || n_classes_ <= 0 || n_classes_ > 4096 || dim_ <= 0) {
    if (err) *err = "bad header"; return false;
  }
  W_.assign(size_t(vocab_) * n_classes_, 0.0f);
  f.read((char*)W_.data(), sizeof(float) * W_.size());
  class_names_.clear();
  for (int i = 0; i < n_classes_; ++i) {
    int32_t len = 0;
    f.read((char*)&len, 4);
    if (len <= 0 || len > 256) { if (err) *err = "bad class name"; return false; }
    std::string s(len, 0);
    f.read(s.data(), len);
    class_names_.push_back(s);
  }
  return bool(f) || class_names_.size() == size_t(n_classes_);
}

void AssetSearchModel::hash_features(const std::string& text, std::vector<float>& feats) const {
  feats.assign(vocab_, 0.0f);
  std::string low = text;
  for (auto& c : low) c = (char)std::tolower(c);
  auto hash_add = [&](const std::string& t) {
    uint64_t h = 1469598103934665603ull;
    for (char c : t) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    size_t idx = h % vocab_;
    feats[idx] += 1.0f;
  };
  // слова + символьные 3-граммы (устойчиво к падежам/опечаткам)
  std::string word;
  for (size_t i = 0; i <= low.size(); ++i) {
    char c = i < low.size() ? low[i] : ' ';
    if (std::isalnum((unsigned char)c)) word += c;
    else if (!word.empty()) { hash_add(word); word.clear(); }
  }
  for (size_t i = 0; i + 2 < low.size(); ++i) {
    if (low[i] == ' ') continue;
    hash_add(low.substr(i, 3));
  }
  float norm = 0;
  for (float v : feats) norm += v * v;
  if (norm > 0) {
    norm = std::sqrt(norm);
    for (float& v : feats) v /= norm;
  }
}

Json AssetSearchModel::search(const std::string& query, const AssetCatalog& cat, int top_k) {
  Json out = Json::object();
  out["query"] = Json::of_str(query);
  Json items = Json::array();
  if (loaded() && !cat.entries.empty()) {
    std::vector<float> feats;
    hash_features(query, feats);
    std::vector<float> scores(n_classes_, 0.0f);
    for (int c = 0; c < n_classes_; ++c) {
      float s = 0;
      for (int v = 0; v < vocab_; ++v) s += feats[v] * W_[size_t(v) * n_classes_ + c];
      scores[c] = s;
    }
    // class→entries группировка
    std::unordered_map<std::string, std::vector<const CatalogEntry*>> by_class;
    for (auto& e : cat.entries) by_class[e.cls].push_back(&e);
    std::vector<int> order(n_classes_);
    for (int i = 0; i < n_classes_; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });
    int added = 0;
    for (int ci : order) {
      auto it = by_class.find(class_names_[ci]);
      if (it == by_class.end() || it->second.empty()) continue;
      for (auto* e : it->second) {
        Json jo = Json::object();
        jo["id"] = Json::of_str(e->id);
        jo["class"] = Json::of_str(e->cls);
        jo["name"] = Json::of_str(e->name);
        jo["verts"] = Json::of_num(e->verts);
        jo["faces"] = Json::of_num(e->faces);
        jo["class_score"] = Json::of_num(scores[ci]);
        items.push(jo);
        if (++added >= top_k) goto done;
      }
    }
  done:;
    out["model"] = Json::of_str("hash-ngram-softmax");
  } else {
    // fallback без весов: точное совпадение подстроки
    std::string low = query;
    for (auto& c : low) c = (char)std::tolower(c);
    for (auto& e : cat.entries) {
      if (e.cls.find(low) != std::string::npos || e.name.find(low) != std::string::npos) {
        Json jo = Json::object();
        jo["id"] = Json::of_str(e.id);
        jo["class"] = Json::of_str(e.cls);
        jo["name"] = Json::of_str(e.name);
        jo["verts"] = Json::of_num(e.verts);
        jo["faces"] = Json::of_num(e.faces);
        items.push(jo);
        if ((int)items.arr.size() >= top_k) break;
      }
    }
    out["model"] = Json::of_str("substring-fallback");
  }
  out["results"] = items;
  out["n_catalog"] = Json::of_num(cat.entries.size());
  return out;
}

// ── Примитивы ──
std::shared_ptr<Mesh> make_box(const Vec3& size) {
  auto m = std::make_shared<Mesh>();
  m->name = "box";
  Vec3 h = size * 0.5f;
  Vec3 p[8] = {{-h.x,-h.y,-h.z},{h.x,-h.y,-h.z},{h.x,h.y,-h.z},{-h.x,h.y,-h.z},
               {-h.x,-h.y,h.z},{h.x,-h.y,h.z},{h.x,h.y,h.z},{-h.x,h.y,h.z}};
  Vec3 n[6] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
  int quad[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{4,5,1,0},{3,2,6,7}};
  for (int f = 0; f < 6; ++f) {
    uint32_t base = (uint32_t)m->pos.size();
    for (int k = 0; k < 4; ++k) { m->pos.push_back(p[quad[f][k]]); m->nrm.push_back(n[f]); m->uv.push_back({}); }
    m->idx.insert(m->idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  m->recompute_bounds();
  return m;
}

std::shared_ptr<Mesh> make_plane(float size) {
  auto m = std::make_shared<Mesh>();
  m->name = "plane";
  float h = size * 0.5f;
  m->pos = {{-h,0,-h},{h,0,-h},{h,0,h},{-h,0,h}};
  for (int i = 0; i < 4; ++i) m->nrm.push_back({0, 1, 0});
  m->idx = {0, 1, 2, 0, 2, 3};
  m->recompute_bounds();
  return m;
}

std::shared_ptr<Mesh> make_sphere(float radius, int seg) {
  auto m = std::make_shared<Mesh>();
  m->name = "sphere";
  int stacks = seg, slices = seg * 2;
  for (int i = 0; i <= stacks; ++i) {
    float phi = PI * i / stacks;
    for (int j = 0; j <= slices; ++j) {
      float th = 2 * PI * j / slices;
      float nx = std::sin(phi) * std::cos(th), ny = std::cos(phi), nz = std::sin(phi) * std::sin(th);
      m->pos.push_back({radius * nx, radius * ny, radius * nz});
      m->nrm.push_back({nx, ny, nz});
      m->uv.push_back({float(j) / slices, float(i) / stacks});
    }
  }
  for (int i = 0; i < stacks; ++i)
    for (int j = 0; j < slices; ++j) {
      uint32_t a = i * (slices + 1) + j, b = a + slices + 1;
      m->idx.insert(m->idx.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  m->recompute_bounds();
  return m;
}

std::shared_ptr<Mesh> make_capsule(float radius, float height) {
  // цилиндр + два полушария (приближённо: вертикальный капсуль)
  auto m = std::make_shared<Mesh>();
  m->name = "capsule";
  int seg = 12;
  float half = height * 0.5f;
  for (int i = 0; i <= seg; ++i) {
    float th = 2 * PI * i / seg;
    float cx = std::cos(th), cz = std::sin(th);
    m->pos.push_back({radius * cx, half, radius * cz});
    m->nrm.push_back({cx, 0, cz});
    m->pos.push_back({radius * cx, -half, radius * cz});
    m->nrm.push_back({cx, 0, cz});
    m->uv.push_back({});
  }
  for (int i = 0; i < seg; ++i) {
    uint32_t a = i * 2;
    m->idx.insert(m->idx.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
  }
  // полусферы
  for (int side = 0; side < 2; ++side) {
    float y0 = side ? half : -half;
    int sign = side ? 1 : -1;
    uint32_t base = (uint32_t)m->pos.size();
    for (int i = 0; i <= seg / 2; ++i) {
      float phi = (PI / 2) * i / (seg / 2);
      for (int j = 0; j <= seg; ++j) {
        float th = 2 * PI * j / seg;
        float nx = std::sin(phi) * std::cos(th), ny = std::cos(phi) * sign, nz = std::sin(phi) * std::sin(th);
        m->pos.push_back({radius * nx, y0 + radius * ny * (side ? 1 : 1), radius * nz});
        m->nrm.push_back({nx, ny, nz});
        m->uv.push_back({});
      }
    }
    for (int i = 0; i < seg / 2; ++i)
      for (int j = 0; j < seg; ++j) {
        uint32_t a = base + i * (seg + 1) + j, b = a + seg + 1;
        if (side == 0) m->idx.insert(m->idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        else m->idx.insert(m->idx.end(), {a, a + 1, b, a + 1, b + 1, b});
      }
  }
  m->recompute_bounds();
  return m;
}

} // namespace af
