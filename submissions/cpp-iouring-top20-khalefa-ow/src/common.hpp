// HTTP/JSON/UTF-8/HS256 helpers, adapted from ../cpp-iouring-khalefa-ow (MIT).
#pragma once
#include <openssl/sha.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace top20 {

constexpr size_t kReadChunk = 4096;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 16 * 1024;
constexpr int kIdleTimeoutS = 120;  // SPEC: keep idle connections at least 65 s
constexpr int64_t kMaxId = INT64_MAX;

// ---------------------------------------------------------------------------------------------
// small helpers

[[noreturn]] void die(const char* what, const char* detail = nullptr) {
  std::fprintf(stderr, "fatal: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
  std::exit(1);
}

int64_t monotonic_s() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

void append_int(std::string& out, int64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr - buf);
}

// Appends s as a JSON string literal (quotes included). Non-ASCII UTF-8 passes through as is.
void append_json_str(std::string& out, const char* s, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  size_t run = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    out.append(s + run, i - run);
    run = i + 1;
    switch (c) {
      case '"': out.append("\\\"", 2); break;
      case '\\': out.append("\\\\", 2); break;
      case '\n': out.append("\\n", 2); break;
      case '\r': out.append("\\r", 2); break;
      case '\t': out.append("\\t", 2); break;
      case '\b': out.append("\\b", 2); break;
      case '\f': out.append("\\f", 2); break;
      default: {
        char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
        out.append(esc, 6);
      }
    }
  }
  out.append(s + run, n - run);
  out.push_back('"');
}
void append_json_str(std::string& out, std::string_view s) { append_json_str(out, s.data(), s.size()); }

// A positive integer in plain decimal digits. Returns 1 if valid, 0 if not a positive integer,
// and 2 if it is valid but beyond the signed 64-bit post-id range.
int parse_id(std::string_view s, int64_t* out) {
  if (s.empty()) return 0;
  int64_t v = 0;
  bool overflow = false;
  for (char ch : s) {
    if (ch < '0' || ch > '9') return 0;
    int d = ch - '0';
    if (!overflow && v > (kMaxId - d) / 10) overflow = true;
    if (!overflow) v = v * 10 + d;
  }
  if (overflow) return 2;
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

// ---------------------------------------------------------------------------------------------
// strict JSON (RFC 8259) parser that extracts selected top-level fields of an object

enum JType : uint8_t { J_NONE, J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct JField {
  JField(std::string_view k) : key(k) {}
  std::string_view key;
  JType type = J_NONE;  // J_NONE: key absent
  double num = 0;
  std::string str;
  std::string_view raw;  // Borrowed JSON span, used only while loading a seed.
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view s) : p_(s.data()), e_(s.data() + s.size()) {}

  // Parses the whole document. If it is an object, `fields` with matching keys are filled in
  // (the last duplicate wins). Returns false if the document is not valid JSON.
  bool parse(JType* top, JField* fields, size_t nfields) {
    ws();
    if (!value(top, nullptr, nullptr, fields, nfields)) return false;
    ws();
    return p_ == e_;
  }

  // Bounded, fully validated array items without a general JSON DOM.
  bool array_items(std::vector<std::string_view>& items, size_t limit) {
    items.clear();
    ws();
    if (p_ == e_ || *p_++ != '[') return false;
    ws();
    if (p_ < e_ && *p_ == ']') { ++p_; ws(); return p_ == e_; }
    for (;;) {
      if (items.size() == limit) return false;
      ws();
      const char* start = p_;
      if (!value(nullptr, nullptr, nullptr, nullptr, 0)) return false;
      items.emplace_back(start, p_ - start);
      ws();
      if (p_ == e_) return false;
      if (*p_ == ']') { ++p_; ws(); return p_ == e_; }
      if (*p_++ != ',') return false;
    }
  }

 private:
  const char* p_;
  const char* e_;
  int depth_ = 0;
  std::string key_;

  void ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool lit(const char* word, size_t n) {
    if (static_cast<size_t>(e_ - p_) < n || std::memcmp(p_, word, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool value(JType* t, double* num, std::string* str, JField* fields, size_t nfields) {
    if (p_ >= e_) return false;
    JType type;
    switch (*p_) {
      case '{':
        type = J_OBJ;
        if (!object(fields, nfields)) return false;
        break;
      case '[':
        type = J_ARR;
        if (!array()) return false;
        break;
      case '"':
        type = J_STR;
        if (!string(str)) return false;
        break;
      case 't':
        type = J_BOOL;
        if (!lit("true", 4)) return false;
        break;
      case 'f':
        type = J_BOOL;
        if (!lit("false", 5)) return false;
        break;
      case 'n':
        type = J_NULL;
        if (!lit("null", 4)) return false;
        break;
      default:
        type = J_NUM;
        if (!number(num)) return false;
    }
    if (t) *t = type;
    return true;
  }

  bool object(JField* fields, size_t nfields) {
    if (++depth_ > 256) return false;
    ++p_;
    ws();
    if (p_ < e_ && *p_ == '}') {
      ++p_;
      --depth_;
      return true;
    }
    for (;;) {
      ws();
      if (p_ >= e_ || *p_ != '"') return false;
      key_.clear();
      if (!string(&key_)) return false;
      JField* f = nullptr;
      for (size_t i = 0; i < nfields; i++)
        if (fields[i].key == key_) f = &fields[i];
      ws();
      if (p_ >= e_ || *p_ != ':') return false;
      ++p_;
      ws();
      if (f) {
        f->str.clear();
        const char* start = p_;
        if (!value(&f->type, &f->num, &f->str, nullptr, 0)) return false;
        f->raw = std::string_view(start, p_ - start);
      } else if (!value(nullptr, nullptr, nullptr, nullptr, 0)) {
        return false;
      }
      ws();
      if (p_ >= e_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ != '}') return false;
      ++p_;
      --depth_;
      return true;
    }
  }

  bool array() {
    if (++depth_ > 256) return false;
    ++p_;
    ws();
    if (p_ < e_ && *p_ == ']') {
      ++p_;
      --depth_;
      return true;
    }
    for (;;) {
      ws();
      if (!value(nullptr, nullptr, nullptr, nullptr, 0)) return false;
      ws();
      if (p_ >= e_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ != ']') return false;
      ++p_;
      --depth_;
      return true;
    }
  }

  bool number(double* out) {
    const char* start = p_;
    if (p_ < e_ && *p_ == '-') ++p_;
    if (p_ >= e_) return false;
    if (*p_ == '0') {
      ++p_;
    } else if (*p_ >= '1' && *p_ <= '9') {
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    } else {
      return false;
    }
    if (p_ < e_ && *p_ == '.') {
      ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (out) {
#if defined(__APPLE__)
      // Older Apple libc++ has only integer from_chars. This path is for portable core checks.
      std::string number_text(start, p_);
      *out = std::strtod(number_text.c_str(), nullptr);
#else
      auto r = std::from_chars(start, p_, *out);
      if (r.ec == std::errc::result_out_of_range) *out = (*start == '-') ? -HUGE_VAL : HUGE_VAL;
#endif
    }
    return true;
  }

  static int hex4(const char* s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
      char c = s[i];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return -1;
      v = v * 16 + d;
    }
    return v;
  }

  static void put_utf8(std::string* out, uint32_t cp) {
    if (!out) return;
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Length of the valid UTF-8 sequence starting at p (lead byte >= 0x80), or 0 if invalid.
  static size_t utf8_seq(const unsigned char* p, const unsigned char* e) {
    unsigned char c = p[0];
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0) n = 3, lo = 0xA0;
    else if (c >= 0xE1 && c <= 0xEC) n = 3;
    else if (c == 0xED) n = 3, hi = 0x9F;
    else if (c >= 0xEE && c <= 0xEF) n = 3;
    else if (c == 0xF0) n = 4, lo = 0x90;
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4) n = 4, hi = 0x8F;
    else return 0;
    if (static_cast<size_t>(e - p) < n) return 0;
    if (p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < n; i++)
      if ((p[i] & 0xC0) != 0x80) return 0;
    return n;
  }

  bool string(std::string* out) {
    ++p_;  // opening quote
    const char* run = p_;
    auto flush = [&] {
      if (out) out->append(run, p_ - run);
    };
    while (p_ < e_) {
      unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        flush();
        ++p_;
        return true;
      }
      if (c < 0x20) return false;
      if (c < 0x80 && c != '\\') {
        ++p_;
        continue;
      }
      if (c >= 0x80) {
        size_t n = utf8_seq(reinterpret_cast<const unsigned char*>(p_),
                            reinterpret_cast<const unsigned char*>(e_));
        if (!n) return false;
        p_ += n;
        continue;
      }
      // backslash escape
      flush();
      if (e_ - p_ < 2) return false;
      char esc = p_[1];
      p_ += 2;
      switch (esc) {
        case '"': if (out) out->push_back('"'); break;
        case '\\': if (out) out->push_back('\\'); break;
        case '/': if (out) out->push_back('/'); break;
        case 'b': if (out) out->push_back('\b'); break;
        case 'f': if (out) out->push_back('\f'); break;
        case 'n': if (out) out->push_back('\n'); break;
        case 'r': if (out) out->push_back('\r'); break;
        case 't': if (out) out->push_back('\t'); break;
        case 'u': {
          if (e_ - p_ < 4) return false;
          int cp = hex4(p_);
          if (cp < 0) return false;
          p_ += 4;
          if (cp >= 0xDC00 && cp <= 0xDFFF) return false;  // lone low surrogate
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
            int lo = hex4(p_ + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            p_ += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(out, static_cast<uint32_t>(cp));
          break;
        }
        default:
          return false;
      }
      run = p_;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------------------------
// body trimming (JavaScript String.prototype.trim()'s whitespace set) and length

bool is_js_space(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

// Decodes the (already validated) UTF-8 code point at s[i], returning its byte length.
size_t decode_utf8(const unsigned char* s, uint32_t* cp) {
  unsigned char c = s[0];
  if (c < 0x80) return *cp = c, 1;
  if (c < 0xE0) return *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F), 2;
  if (c < 0xF0) return *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F), 3;
  *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  return 4;
}

std::string_view trim_js(std::string_view s) {
  auto* b = reinterpret_cast<const unsigned char*>(s.data());
  size_t lo = 0, hi = s.size();
  while (lo < hi) {
    uint32_t cp;
    size_t n = decode_utf8(b + lo, &cp);
    if (!is_js_space(cp)) break;
    lo += n;
  }
  while (hi > lo) {
    size_t start = hi - 1;
    while (start > lo && (b[start] & 0xC0) == 0x80) --start;
    uint32_t cp;
    decode_utf8(b + start, &cp);
    if (!is_js_space(cp)) break;
    hi = start;
  }
  return s.substr(lo, hi - lo);
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

// ---------------------------------------------------------------------------------------------
// JWT (HS256 only). The HMAC key pads are hashed once at startup; every token is still fully
// verified on every request (rule 5).

SHA256_CTX g_hmac_inner, g_hmac_outer;

void hmac_init(std::string_view key) {
  unsigned char k[SHA256_CBLOCK] = {0};
  if (key.size() > SHA256_CBLOCK) {
    SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), k);
  } else {
    std::memcpy(k, key.data(), key.size());
  }
  unsigned char ipad[SHA256_CBLOCK], opad[SHA256_CBLOCK];
  for (int i = 0; i < SHA256_CBLOCK; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  SHA256_Init(&g_hmac_inner);
  SHA256_Update(&g_hmac_inner, ipad, sizeof ipad);
  SHA256_Init(&g_hmac_outer);
  SHA256_Update(&g_hmac_outer, opad, sizeof opad);
}

void hmac_sha256(const char* msg, size_t n, unsigned char out[32]) {
  unsigned char inner[32];
  SHA256_CTX c = g_hmac_inner;
  SHA256_Update(&c, msg, n);
  SHA256_Final(inner, &c);
  c = g_hmac_outer;
  SHA256_Update(&c, inner, sizeof inner);
  SHA256_Final(out, &c);
}

const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int8_t b64url_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool b64url_decode(std::string_view in, std::string& out) {
  out.clear();
  if (in.size() % 4 == 1) return false;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    int v = b64url_value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

size_t b64url_encode(const unsigned char* in, size_t n, char* out) {
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
    out[o++] = kB64Url[(v >> 6) & 63];
    out[o++] = kB64Url[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = in[i] << 16;
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
  } else if (n - i == 2) {
    uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
    out[o++] = kB64Url[(v >> 6) & 63];
  }
  return o;
}

enum class AuthResult { kOk, kMissing, kInvalid, kBadPayload };

struct AuthUser {
  int64_t id = 0;
  std::string username;
};

AuthResult authenticate(std::string_view header, bool present, AuthUser* user) {
  if (!present || header.size() < 7 || header.substr(0, 7) != "Bearer ") return AuthResult::kMissing;
  std::string_view token = header.substr(7);
  size_t d1 = token.find('.');
  if (d1 == std::string_view::npos) return AuthResult::kInvalid;
  size_t d2 = token.find('.', d1 + 1);
  if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos)
    return AuthResult::kInvalid;
  std::string_view head_b64 = token.substr(0, d1);
  std::string_view payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
  std::string_view sig_b64 = token.substr(d2 + 1);

  static std::string decoded;
  JType top;
  JField alg[1] = {{"alg"}};
  if (!b64url_decode(head_b64, decoded) || !JsonParser(decoded).parse(&top, alg, 1) || top != J_OBJ ||
      alg[0].type != J_STR || alg[0].str != "HS256")
    return AuthResult::kInvalid;

  unsigned char mac[32];
  hmac_sha256(token.data(), d2, mac);
  char expected[44];
  size_t elen = b64url_encode(mac, sizeof mac, expected);
  if (sig_b64.size() != elen) return AuthResult::kInvalid;
  unsigned char diff = 0;
  for (size_t i = 0; i < elen; i++) diff |= static_cast<unsigned char>(expected[i] ^ sig_b64[i]);
  if (diff) return AuthResult::kInvalid;

  enum { kSub, kUsername, kExp, kNbf };
  JField f[4] = {{"sub"}, {"username"}, {"exp"}, {"nbf"}};
  if (!b64url_decode(payload_b64, decoded) || !JsonParser(decoded).parse(&top, f, 4) || top != J_OBJ)
    return AuthResult::kInvalid;
  double now = static_cast<double>(time(nullptr));
  JType exp_t = f[kExp].type, nbf_t = f[kNbf].type;
  if (exp_t != J_NONE && (exp_t != J_NUM || !std::isfinite(f[kExp].num) || now >= f[kExp].num))
    return AuthResult::kInvalid;
  if (nbf_t != J_NONE && (nbf_t != J_NUM || !std::isfinite(f[kNbf].num) || now < f[kNbf].num))
    return AuthResult::kInvalid;

  if (f[kSub].type != J_STR || parse_id(f[kSub].str, &user->id) != 1 || f[kUsername].type != J_STR)
    return AuthResult::kBadPayload;
  user->username = std::move(f[kUsername].str);
  return AuthResult::kOk;
}

int64_t g_start_s;

int error(std::string& body, int status, const char* msg) {
  body.append("{\"error\":\"");
  body.append(msg);
  body.append("\"}");
  return status;
}

int auth_error(std::string& b, AuthResult r) {
  switch (r) {
    case AuthResult::kMissing: return error(b, 401, "missing bearer token");
    case AuthResult::kBadPayload: return error(b, 401, "invalid token payload");
    default: return error(b, 401, "invalid or expired token");
  }
}

// ---------------------------------------------------------------------------------------------
// HTTP

struct Request {
  std::string_view method;
  std::string_view path;
  std::string_view auth;
  bool has_auth = false;
  std::string_view body;
  bool keep_alive = true;
};

const char* status_line(int status) {
  switch (status) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 413: return "HTTP/1.1 413 Payload Too Large\r\n";
    case 431: return "HTTP/1.1 431 Request Header Fields Too Large\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

void append_response(std::string& out, int status, const std::string& body, bool close) {
  out.append(status_line(status));
  out.append("Content-Type: application/json\r\nContent-Length: ");
  append_int(out, static_cast<int64_t>(body.size()));
  out.append(close ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n");
  out.append(body);
}

bool ieq(std::string_view a, std::string_view lower) {
  if (a.size() != lower.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c != lower[i]) return false;
  }
  return true;
}

bool icontains(std::string_view hay, std::string_view lower) {
  if (hay.size() < lower.size()) return false;
  for (size_t i = 0; i + lower.size() <= hay.size(); i++)
    if (ieq(hay.substr(i, lower.size()), lower)) return true;
  return false;
}

std::string_view trim_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

enum class Parse { kOk, kIncomplete, kBad, kHeadersTooLarge, kBodyTooLarge };

// Decodes a chunked body starting at p. Returns bytes consumed, 0 if incomplete, -1 if malformed.
ssize_t decode_chunked(const char* p, const char* e, std::string& out) {
  const char* start = p;
  out.clear();
  for (;;) {
    const char* eol = static_cast<const char*>(memmem(p, e - p, "\r\n", 2));
    if (!eol) return 0;
    size_t size = 0;
    const char* q = p;
    for (; q < eol && *q != ';'; q++) {
      char c = *q;
      int v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else return -1;
      size = size * 16 + v;
      if (size > kMaxBodyBytes) return -1;
    }
    if (q == p) return -1;
    p = eol + 2;
    if (size == 0) {
      // optional trailers, then an empty line
      for (;;) {
        const char* end = static_cast<const char*>(memmem(p, e - p, "\r\n", 2));
        if (!end) return 0;
        bool empty = end == p;
        p = end + 2;
        if (empty) return p - start;
      }
    }
    if (static_cast<size_t>(e - p) < size + 2) return 0;
    if (p[size] != '\r' || p[size + 1] != '\n') return -1;
    out.append(p, size);
    if (out.size() > kMaxBodyBytes) return -1;
    p += size + 2;
  }
}

// Parses one request from [data, data+len). On kOk, *consumed is its total length.
Parse parse_request(const char* data, size_t len, Request* r, size_t* consumed, std::string& chunked) {
  const char* end = static_cast<const char*>(memmem(data, len, "\r\n\r\n", 4));
  if (!end) return len > kMaxHeaderBytes ? Parse::kHeadersTooLarge : Parse::kIncomplete;
  size_t head_len = end - data + 4;
  if (head_len > kMaxHeaderBytes) return Parse::kHeadersTooLarge;
  std::string_view head(data, end - data + 2);  // includes the last header line's CRLF

  size_t eol = head.find("\r\n");
  std::string_view line = head.substr(0, eol);
  size_t sp1 = line.find(' ');
  size_t sp2 = line.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == sp1) return Parse::kBad;
  r->method = line.substr(0, sp1);
  r->path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string_view version = line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return Parse::kBad;
  if (r->method.empty() || r->path.empty() || r->path.front() != '/') return Parse::kBad;
  for (unsigned char c : r->path) if (c <= 0x20 || c == 0x7f) return Parse::kBad;
  bool http10 = version == "HTTP/1.0";
  r->keep_alive = !http10;
  r->has_auth = false;
  r->auth = {};
  r->body = {};

  size_t content_length = 0;
  bool have_length = false;
  bool have_transfer = false;
  bool chunked_te = false;
  size_t pos = eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    std::string_view h = head.substr(pos, next - pos);
    pos = next + 2;
    size_t colon = h.find(':');
    if (colon == std::string_view::npos) return Parse::kBad;
    std::string_view name = h.substr(0, colon);
    std::string_view value = trim_ows(h.substr(colon + 1));
    if (name.empty()) return Parse::kBad;
    for (unsigned char c : name) {
      bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
      if (!token) return Parse::kBad;
    }
    for (unsigned char c : value) if ((c < 0x20 && c != '\t') || c == 0x7f) return Parse::kBad;
    switch (name.size()) {
      case 13:
        if (ieq(name, "authorization")) {
          if (r->has_auth) return Parse::kBad;
          r->auth = value;
          r->has_auth = true;
        }
        break;
      case 14:
        if (ieq(name, "content-length")) {
          if (have_length) return Parse::kBad;
          have_length = true;
          if (value.empty()) return Parse::kBad;
          size_t v = 0;
          for (char c : value) {
            if (c < '0' || c > '9') return Parse::kBad;
            v = v * 10 + (c - '0');
            if (v > kMaxBodyBytes) return Parse::kBodyTooLarge;
          }
          content_length = v;
        }
        break;
      case 10:
        if (ieq(name, "connection")) {
          if (icontains(value, "close")) r->keep_alive = false;
          else if (icontains(value, "keep-alive")) r->keep_alive = true;
        }
        break;
      case 17:
        if (ieq(name, "transfer-encoding")) {
          if (have_transfer || !ieq(value, "chunked")) return Parse::kBad;
          have_transfer = true;
          chunked_te = true;
        }
        break;
    }
  }

  if (have_length && have_transfer) return Parse::kBad;
  if (chunked_te) {
    ssize_t n = decode_chunked(data + head_len, data + len, chunked);
    if (n < 0) return Parse::kBad;
    if (n == 0) return len - head_len > kMaxBodyBytes + 64 * 1024 ? Parse::kBodyTooLarge : Parse::kIncomplete;
    r->body = chunked;
    *consumed = head_len + n;
    return Parse::kOk;
  }
  if (len - head_len < content_length) return Parse::kIncomplete;
  r->body = std::string_view(data + head_len, content_length);
  *consumed = head_len + content_length;
  return Parse::kOk;
}


}  // namespace top20
