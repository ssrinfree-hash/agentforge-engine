// AgentForge — собственный JSON (парсер + сериализатор). C++17.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace af {

class Json {
public:
  enum class Type { Null, Bool, Num, Str, Arr, Obj };
  Type type = Type::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;   // ordered by key — стабильный вывод

  Json() = default;
  static Json object() { Json j; j.type = Type::Obj; return j; }
  static Json array()  { Json j; j.type = Type::Arr; return j; }
  static Json of_num(double v) { Json j; j.type = Type::Num; j.num = v; return j; }
  static Json of_str(const std::string& s) { Json j; j.type = Type::Str; j.str = s; return j; }
  static Json of_bool(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }

  bool is_null() const { return type == Type::Null; }
  bool is_obj()  const { return type == Type::Obj; }
  bool is_arr()  const { return type == Type::Arr; }
  bool is_num()  const { return type == Type::Num; }
  bool is_str()  const { return type == Type::Str; }
  bool is_bool() const { return type == Type::Bool; }

  Json& operator[](const std::string& k) {
    if (type != Type::Obj) { type = Type::Obj; arr.clear(); }
    return obj[k];
  }
  const Json* find(const std::string& k) const {
    if (type != Type::Obj) return nullptr;
    auto it = obj.find(k);
    return it == obj.end() ? nullptr : &it->second;
  }
  void push(const Json& v) { if (type != Type::Arr) { type = Type::Arr; obj.clear(); } arr.push_back(v); }

  double as_num(double def = 0) const { return is_num() ? num : def; }
  std::string as_str(const std::string& def = "") const { return is_str() ? str : def; }
  bool as_bool(bool def = false) const { return is_bool() ? b : def; }

  std::string dump(int indent = 0) const;
  static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

private:
  void dump_impl(std::string& out, int indent, int depth) const;
  static void escape(const std::string& s, std::string& out);
};

} // namespace af
