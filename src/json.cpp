// AgentForge — реализация JSON.
#include "af/json.h"
#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstring>

namespace af {

void Json::escape(const std::string& s, std::string& out) {
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else out += c;
    }
  }
}

void Json::dump_impl(std::string& out, int indent, int depth) const {
  auto pad = [&](int d) { if (indent > 0) { out += '\n'; out.append(static_cast<size_t>(indent) * d, ' '); } };
  switch (type) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b ? "true" : "false"; break;
    case Type::Num: {
      if (std::isfinite(num)) {
        if (num == static_cast<long long>(num) && std::fabs(num) < 1e15)
          out += std::to_string(static_cast<long long>(num));
        else {
          char buf[40]; std::snprintf(buf, sizeof buf, "%.6g", num); out += buf;
        }
      } else out += "null";
      break;
    }
    case Type::Str: out += '"'; escape(str, out); out += '"'; break;
    case Type::Arr: {
      if (arr.empty()) { out += "[]"; break; }
      out += '[';
      for (size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ',';
        pad(depth + 1);
        arr[i].dump_impl(out, indent, depth + 1);
      }
      pad(depth); out += ']';
      break;
    }
    case Type::Obj: {
      if (obj.empty()) { out += "{}"; break; }
      out += '{';
      bool first = true;
      for (auto& [k, v] : obj) {
        if (!first) out += ',';
        first = false;
        pad(depth + 1);
        out += '"'; escape(k, out); out += "\":";
        v.dump_impl(out, indent, depth + 1);
      }
      pad(depth); out += '}';
      break;
    }
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_impl(out, indent, 0);
  return out;
}

namespace {
struct P {
  const char* s; size_t n, i; std::string err;
  bool fail(const std::string& m) { if (err.empty()) err = m + " (offset " + std::to_string(i) + ")"; return false; }
  void ws() { while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  bool lit(const char* l) {
    size_t L = std::strlen(l);
    if (i + L <= n && std::strncmp(s + i, l, L) == 0) { i += L; return true; }
    return false;
  }
  bool value(Json& out) {
    ws(); if (i >= n) return fail("unexpected end");
    char c = s[i];
    if (c == '{') return object(out);
    if (c == '[') return array(out);
    if (c == '"') return string(out);
    if (lit("true"))  { out = Json::of_bool(true); return true; }
    if (lit("false")) { out = Json::of_bool(false); return true; }
    if (lit("null"))  { out = Json(); return true; }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return number(out);
    return fail(std::string("unexpected char '") + c + "'");
  }
  bool number(Json& out) {
    char* endp = nullptr;
    double v = std::strtod(s + i, &endp);
    if (endp == s + i) return fail("bad number");
    i = static_cast<size_t>(endp - s);
    out = Json::of_num(v);
    return true;
  }
  bool string(Json& out) {
    if (s[i] != '"') return fail("expected '\"'");
    ++i;
    std::string r;
    while (i < n) {
      char c = s[i++];
      if (c == '"') { out = Json::of_str(r); return true; }
      if (c == '\\') {
        if (i >= n) return fail("bad escape");
        char e = s[i++];
        switch (e) {
          case '"': r += '"'; break; case '\\': r += '\\'; break;
          case '/': r += '/'; break; case 'n': r += '\n'; break;
          case 't': r += '\t'; break; case 'r': r += '\r'; break;
          case 'b': r += '\b'; break; case 'f': r += '\f'; break;
          case 'u': {
            if (i + 4 > n) return fail("bad \\u");
            unsigned cp = static_cast<unsigned>(std::strtoul(std::string(s + i, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp < 0x80) r += static_cast<char>(cp);
            else if (cp < 0x800) { r += static_cast<char>(0xC0 | (cp >> 6)); r += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { r += static_cast<char>(0xE0 | (cp >> 12)); r += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); r += static_cast<char>(0x80 | (cp & 0x3F)); }
            break;
          }
          default: return fail("bad escape char");
        }
      } else r += c;
    }
    return fail("unterminated string");
  }
  bool array(Json& out) {
    ++i; out = Json::array(); ws();
    if (i < n && s[i] == ']') { ++i; return true; }
    while (true) {
      Json v; if (!value(v)) return false;
      out.arr.push_back(std::move(v));
      ws();
      if (i < n && s[i] == ',') { ++i; continue; }
      if (i < n && s[i] == ']') { ++i; return true; }
      return fail("expected ',' or ']'");
    }
  }
  bool object(Json& out) {
    ++i; out = Json::object(); ws();
    if (i < n && s[i] == '}') { ++i; return true; }
    while (true) {
      ws();
      Json k; if (i >= n || s[i] != '"') return fail("expected key");
      if (!string(k)) return false;
      ws();
      if (i >= n || s[i] != ':') return fail("expected ':'");
      ++i;
      Json v; if (!value(v)) return false;
      out.obj[k.str] = std::move(v);
      ws();
      if (i < n && s[i] == ',') { ++i; continue; }
      if (i < n && s[i] == '}') { ++i; return true; }
      return fail("expected ',' or '}'");
    }
  }
};
} // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
  P p{text.c_str(), text.size(), 0, ""};
  if (!p.value(out)) { if (err) *err = p.err; return false; }
  p.ws();
  if (p.i != p.n) { if (err) *err = "trailing data (offset " + std::to_string(p.i) + ")"; return false; }
  return true;
}

} // namespace af
