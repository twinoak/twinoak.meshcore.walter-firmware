#include "sim/scripted_server.h"

#include <cstdio>
#include <cstring>

namespace sim {

namespace {

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string trim(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

}  // namespace

bool SeenRequest::has(const char* name) const { return headers.count(lower(name)) != 0; }

std::string SeenRequest::header(const char* name) const {
  const auto it = headers.find(lower(name));
  return it == headers.end() ? std::string() : it->second;
}

ScriptedServer::ScriptedServer() {
  hosts.push_back(ScriptedHost{"server.example", core::net::kRootIsrgX1});
}

bool ScriptedServer::rootOf(const std::string& host, std::uint8_t* root) const {
  for (const ScriptedHost& h : hosts) {
    if (lower(h.name) == lower(host)) {
      if (root != nullptr) *root = h.root;
      return true;
    }
  }
  return false;
}

void ScriptedServer::serve(const std::string& path, const std::vector<std::uint8_t>& bytes,
                           const std::string& etag, const std::string& last_modified) {
  ServedFile f;
  f.bytes = bytes;
  f.etag = etag;
  f.last_modified = last_modified;
  files[path] = f;
}

std::string ScriptedServer::statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Whatever";
  }
}

void ScriptedServer::append(std::vector<std::uint8_t>& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
}

std::vector<std::uint8_t> ScriptedServer::response(int status, const std::vector<std::string>& headers,
                                                   const std::uint8_t* body, std::size_t body_size,
                                                   bool with_length) {
  std::vector<std::uint8_t> out;
  append(out, "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n");
  append(out, "Server: scripted/1\r\n");
  for (const std::string& h : headers) append(out, h + "\r\n");
  if (with_length) append(out, "Content-Length: " + std::to_string(body_size) + "\r\n");
  append(out, "Connection: close\r\n\r\n");
  if (body != nullptr && body_size != 0) out.insert(out.end(), body, body + body_size);
  return out;
}

std::vector<std::uint8_t> ScriptedServer::chunkedResponse(int status, const std::vector<std::string>& headers,
                                                          const std::uint8_t* body, std::size_t body_size,
                                                          std::size_t chunk) {
  std::vector<std::uint8_t> out;
  append(out, "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n");
  for (const std::string& h : headers) append(out, h + "\r\n");
  append(out, "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  if (chunk == 0) chunk = 1;
  for (std::size_t at = 0; at < body_size;) {
    const std::size_t n = body_size - at < chunk ? body_size - at : chunk;
    char size[32];
    std::snprintf(size, sizeof size, "%zx\r\n", n);
    append(out, size);
    out.insert(out.end(), body + at, body + at + n);
    append(out, "\r\n");
    at += n;
  }
  append(out, "0\r\n\r\n");
  return out;
}

std::vector<std::uint8_t> ScriptedServer::respond(const std::string& text) {
  // The request line and the headers; the body by Content-Length.
  SeenRequest r;
  std::size_t at = 0;
  std::size_t eol = text.find("\r\n");
  if (eol == std::string::npos) eol = text.size();
  {
    const std::string line = text.substr(0, eol);
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      r.method = line.substr(0, sp1);
      r.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
  }
  at = eol + 2;
  while (at < text.size()) {
    eol = text.find("\r\n", at);
    if (eol == std::string::npos) eol = text.size();
    const std::string line = text.substr(at, eol - at);
    at = eol + 2;
    if (line.empty()) break;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    r.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  r.host = r.header("Host");
  if (at < text.size()) r.body = text.substr(at);
  requests.push_back(r);

  if (no_response) return {};
  if (!raw_response.empty()) return std::vector<std::uint8_t>(raw_response.begin(), raw_response.end());

  // The path without its query.
  std::string path = r.path;
  const std::size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);

  const auto redirect = redirects.find(path);
  if (redirect != redirects.end()) {
    return response(redirect->second.status, {"Location: " + redirect->second.location}, nullptr, 0, true);
  }
  const auto rule = statuses.find(path);
  if (rule != statuses.end()) {
    std::vector<std::string> headers;
    if (!rule->second.retry_after.empty()) headers.push_back("Retry-After: " + rule->second.retry_after);
    headers.push_back("Content-Type: text/plain");
    return response(rule->second.status, headers,
                    reinterpret_cast<const std::uint8_t*>(rule->second.body.data()),
                    rule->second.body.size(), true);
  }
  const auto file = files.find(path);
  if (file == files.end()) {
    static const char kNotFound[] = "not here\n";
    return response(404, {"Content-Type: text/plain"}, reinterpret_cast<const std::uint8_t*>(kNotFound),
                    sizeof kNotFound - 1, true);
  }
  const ServedFile& f = file->second;
  std::vector<std::uint8_t> body = f.bytes;
  for (int i = 0; i < extra_bytes; ++i) body.push_back(0xEE);
  std::vector<std::string> headers;
  headers.push_back("Content-Type: " + f.content_type);
  if (!f.etag.empty()) headers.push_back("ETag: " + f.etag);
  if (!f.last_modified.empty()) headers.push_back("Last-Modified: " + f.last_modified);
  headers.push_back("Accept-Ranges: bytes");

  // A range, when the server honours ranges and the request asked for one, and If-Range (when
  // present and honoured) names the file as it is now.
  if (ranges && r.has("Range") && r.method == "GET") {
    bool ignore = false;
    if (honour_if_range && r.has("If-Range")) {
      const std::string v = r.header("If-Range");
      if (v != f.etag && v != f.last_modified) ignore = true;  // the file changed: send the whole
    }
    if (!ignore) {
      const std::string spec = r.header("Range");
      unsigned long a = 0, b = 0;
      if (std::sscanf(spec.c_str(), "bytes=%lu-%lu", &a, &b) == 2) {
        if (a >= body.size()) {
          headers.push_back("Content-Range: bytes */" + std::to_string(body.size()));
          return response(416, headers, nullptr, 0, true);
        }
        if (b >= body.size()) b = body.size() - 1;
        const unsigned long first = wrong_content_range ? a + 1 : a;
        headers.push_back("Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(b) + "/" +
                          std::to_string(body.size()));
        return response(206, headers, body.data() + a, b - a + 1, true);
      }
    }
  }
  if (f.chunked) return chunkedResponse(200, headers, body.data(), body.size(), f.chunk_size);
  if (f.no_length) return response(200, headers, body.data(), body.size(), false);
  return response(200, headers, body.data(), body.size(), true);
}

}  // namespace sim
