#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <vector>

struct Category {
    int id = 0;
    std::string name;
    std::string slug;
};

struct Game {
    int id = 0;
    std::string title;
    std::string category;
    int categoryId = 0;
    std::string version;
    std::string author;
    std::string description;
    std::string size;
    uint64_t sizeBytes = 0;
    std::string contentId;
    uint32_t contentType = 0x1a;
    uint32_t contentFlags = 0;
    std::string url;   // PKG URL
    std::string image; // Cover URL
    long long updatedAt = 0;
};

struct CatalogData {
    std::string storeName = "WILLGAME Store";
    std::string storeSubtitle = "Catálogo Homebrew PS4";
    long long updatedAt = 0;
    std::vector<Category> categories;
    std::vector<Game> items;
};

inline std::string readFile(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f))) s.append(b, n);
    fclose(f);
    return s;
}

inline bool writeFile(const std::string& p, const std::string& s) {
    FILE* f = fopen((p + ".tmp").c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    if (fclose(f)) ok = false;
    if (ok) ok = rename((p + ".tmp").c_str(), p.c_str()) == 0;
    return ok;
}

inline std::string lower(std::string s) {
    for (char& c : s) {
        if ((unsigned char)c < 128) c = (char)tolower((unsigned char)c);
    }
    return s;
}

inline std::string trimStr(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string cleanField(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return s;
}

inline std::string utf8Encode(unsigned cp) {
    std::string o;
    if (cp < 0x80) {
        o += char(cp);
    } else if (cp < 0x800) {
        o += char(0xc0 | (cp >> 6));
        o += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        o += char(0xe0 | (cp >> 12));
        o += char(0x80 | ((cp >> 6) & 0x3f));
        o += char(0x80 | (cp & 0x3f));
    } else if (cp <= 0x10ffff) {
        o += char(0xf0 | (cp >> 18));
        o += char(0x80 | ((cp >> 12) & 0x3f));
        o += char(0x80 | ((cp >> 6) & 0x3f));
        o += char(0x80 | (cp & 0x3f));
    }
    return o;
}

inline std::string bytesText(uint64_t n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)n;
    int i = 0;
    while (value >= 1024.0 && i < 4) {
        value /= 1024.0;
        ++i;
    }
    char s[48];
    snprintf(s, sizeof s, i ? "%.1f %s" : "%.0f %s", value, u[i]);
    return s;
}

inline double sizeMB(std::string s) {
    std::replace(s.begin(), s.end(), ',', '.');
    double n = atof(s.c_str());
    std::string l = lower(s);
    if (l.find("gb") != std::string::npos) return n * 1024.0;
    if (l.find("kb") != std::string::npos) return n / 1024.0;
    return n;
}

inline bool isHttpUrl(const std::string& url) {
    if (url.empty() || url.size() > 2300) return false;
    if (url.find_first_of(" \t\r\n") != std::string::npos) return false;
    size_t p = url.compare(0, 8, "https://") == 0 ? 8 : url.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!p) return false;
    auto end = url.find_first_of("/?#", p);
    auto host = url.substr(p, end - p);
    return !host.empty() && host[0] != ':' && host != "." && host != ".." && host.find('@') == std::string::npos;
}

inline std::string normalizeServerUrl(std::string raw) {
    raw = trimStr(raw);
    if (raw.empty()) return "";
    if (raw.find("://") == std::string::npos) {
        raw = "http://" + raw;
    }
    while (raw.size() > 8 && raw.back() == '/') raw.pop_back();
    return isHttpUrl(raw) ? raw : "";
}

inline std::string resolveServerUrl(const std::string& serverBase, const std::string& pathOrUrl) {
    std::string u = trimStr(pathOrUrl);
    if (u.empty()) return "";
    if (isHttpUrl(u)) return u;
    std::string base = normalizeServerUrl(serverBase);
    if (base.empty()) return "";
    if (u[0] == '/') return base + u;
    return base + "/" + u;
}

inline std::string readDefaultServerUrl(const std::string& baseAssets, const std::string& dataDir) {
    std::string saved = normalizeServerUrl(readFile(dataDir + "/server.txt"));
    if (!saved.empty()) return saved;
    std::string bundled = normalizeServerUrl(readFile(baseAssets + "/server.txt"));
    if (!bundled.empty()) return bundled;
    return "http://192.168.1.2:8765";
}

inline std::string urlHostLabel(const std::string& url) {
    size_t p = url.compare(0, 8, "https://") == 0 ? 8 : url.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!p) return url;
    auto end = url.find_first_of("/?#", p);
    return url.substr(p, end - p);
}

inline bool endsWithJson(const std::string& url) {
    size_t q = url.find_first_of("?#");
    std::string path = lower(q == std::string::npos ? url : url.substr(0, q));
    return path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0;
}

inline std::string serverBaseFromCatalogUrl(const std::string& url) {
    std::string u = normalizeServerUrl(url);
    if (u.empty()) return "";
    if (!endsWithJson(u)) return u;
    size_t q = u.find_first_of("?#");
    std::string noQuery = (q == std::string::npos) ? u : u.substr(0, q);
    size_t scheme = noQuery.find("://");
    size_t lastSlash = noQuery.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > scheme + 2) {
        return noQuery.substr(0, lastSlash);
    }
    return noQuery;
}

inline uint32_t pkgBig32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline uint64_t pkgBig64(const unsigned char* p) {
    return (uint64_t(pkgBig32(p)) << 32) | pkgBig32(p + 4);
}

inline bool validPkgContentId(const std::string& cid) {
    if (cid.size() != 36) return false;
    for (unsigned char c : cid) {
        if (!(isalnum(c) || c == '-' || c == '_')) return false;
    }
    return cid[6] == '-' && cid[16] == '_' && cid[19] == '-';
}

inline bool parsePkgHeaderBytes(const unsigned char* h, size_t len,
                                std::string& contentId, std::string& titleId,
                                uint64_t& sizeBytes, uint32_t& contentType,
                                uint32_t& contentFlags, std::string& error) {
    if (!h || len < 0x440) {
        error = "Cabeçalho PKG incompleto";
        return false;
    }
    if (pkgBig32(h) != 0x7f434e54u) {
        error = "Arquivo não é um PKG de PS4 válido";
        return false;
    }
    std::string cid((const char*)h + 0x40, 36);
    if (!validPkgContentId(cid)) {
        error = "Content ID inválido no PKG";
        return false;
    }
    uint32_t type = pkgBig32(h + 0x74);
    if (type != 0x1a && type != 0x1b && type != 0x1c && type != 0x1e) {
        error = "Tipo de pacote PS4 não suportado";
        return false;
    }
    uint64_t size = pkgBig64(h + 0x430);
    uint64_t body = pkgBig64(h + 0x20);
    uint64_t bodySize = pkgBig64(h + 0x28);
    if (body > size || bodySize > size - body) {
        error = "Faixa de corpo inválida no PKG";
        return false;
    }
    contentId = cid;
    titleId = cid.substr(7, 9);
    sizeBytes = size;
    contentType = type;
    contentFlags = pkgBig32(h + 0x78);
    return true;
}

namespace MiniJson {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolVal = false;
    double numVal = 0.0;
    uint64_t uintVal = 0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::map<std::string, Value> objVal;

    const Value* get(const std::string& k) const {
        if (type != Object) return nullptr;
        auto it = objVal.find(k);
        return it == objVal.end() ? nullptr : &it->second;
    }
    std::string asString(const std::string& def = "") const {
        return type == String ? strVal : def;
    }
    uint64_t asUInt(uint64_t def = 0) const {
        if (type == Number) return uintVal;
        if (type == String && !strVal.empty()) return strtoull(strVal.c_str(), nullptr, 10);
        return def;
    }
    long long asInt(long long def = 0) const {
        if (type == Number) return (long long)numVal;
        if (type == String && !strVal.empty()) return strtoll(strVal.c_str(), nullptr, 10);
        return def;
    }
};

struct Parser {
    const std::string& src;
    size_t pos;

    explicit Parser(const std::string& s) : src(s), pos(0) {}

    void skipWs() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) {
            ++pos;
        }
    }

    static bool hexDigit(char c, unsigned& out) {
        if (c >= '0' && c <= '9') { out = (unsigned)(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { out = (unsigned)(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { out = (unsigned)(c - 'A' + 10); return true; }
        return false;
    }

    bool readHex4(unsigned& cp) {
        if (pos + 4 > src.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            unsigned d = 0;
            if (!hexDigit(src[pos++], d)) return false;
            cp = (cp << 4) | d;
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (pos >= src.size() || src[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= src.size()) return false;
                char esc = src[pos++];
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!readHex4(cp)) return false;
                        if (cp >= 0xd800 && cp <= 0xdbff && pos + 2 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                            size_t saved = pos;
                            pos += 2;
                            unsigned low = 0;
                            if (readHex4(low) && low >= 0xdc00 && low <= 0xdfff) {
                                cp = 0x10000u + (((cp - 0xd800u) << 10) | (low - 0xdc00u));
                            } else {
                                pos = saved;
                            }
                        }
                        out += utf8Encode(cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    bool parseNumber(Value& v) {
        size_t start = pos;
        if (src[pos] == '-') ++pos;
        if (pos >= src.size() || !isdigit((unsigned char)src[pos])) return false;
        while (pos < src.size() && isdigit((unsigned char)src[pos])) ++pos;
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            if (pos >= src.size() || !isdigit((unsigned char)src[pos])) return false;
            while (pos < src.size() && isdigit((unsigned char)src[pos])) ++pos;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
            if (pos >= src.size() || !isdigit((unsigned char)src[pos])) return false;
            while (pos < src.size() && isdigit((unsigned char)src[pos])) ++pos;
        }
        std::string token = src.substr(start, pos - start);
        v.type = Value::Number;
        v.numVal = strtod(token.c_str(), nullptr);
        v.uintVal = token[0] == '-' ? 0 : strtoull(token.c_str(), nullptr, 10);
        return true;
    }

    bool parseValue(Value& v, int depth = 0) {
        if (depth > 32) return false;
        skipWs();
        if (pos >= src.size()) return false;
        char c = src[pos];
        if (c == '"') {
            v.type = Value::String;
            return parseString(v.strVal);
        }
        if (c == '{') {
            v.type = Value::Object;
            ++pos;
            skipWs();
            if (pos < src.size() && src[pos] == '}') { ++pos; return true; }
            for (;;) {
                skipWs();
                std::string key;
                if (!parseString(key)) return false;
                skipWs();
                if (pos >= src.size() || src[pos] != ':') return false;
                ++pos;
                Value val;
                if (!parseValue(val, depth + 1)) return false;
                v.objVal[key] = std::move(val);
                skipWs();
                if (pos >= src.size()) return false;
                if (src[pos] == '}') { ++pos; return true; }
                if (src[pos] != ',') return false;
                ++pos;
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++pos;
            skipWs();
            if (pos < src.size() && src[pos] == ']') { ++pos; return true; }
            for (;;) {
                Value elem;
                if (!parseValue(elem, depth + 1)) return false;
                v.arrVal.push_back(std::move(elem));
                skipWs();
                if (pos >= src.size()) return false;
                if (src[pos] == ']') { ++pos; return true; }
                if (src[pos] != ',') return false;
                ++pos;
            }
        }
        if (c == 't' && src.compare(pos, 4, "true") == 0) {
            pos += 4; v.type = Value::Bool; v.boolVal = true; return true;
        }
        if (c == 'f' && src.compare(pos, 5, "false") == 0) {
            pos += 5; v.type = Value::Bool; v.boolVal = false; return true;
        }
        if (c == 'n' && src.compare(pos, 4, "null") == 0) {
            pos += 4; v.type = Value::Null; return true;
        }
        if (c == '-' || isdigit((unsigned char)c)) {
            return parseNumber(v);
        }
        return false;
    }
};

inline bool parse(const std::string& jsonText, Value& out) {
    Parser p(jsonText);
    if (!p.parseValue(out)) return false;
    p.skipWs();
    return p.pos == jsonText.size();
}

} // namespace MiniJson

inline std::string firstJsonString(const MiniJson::Value& obj, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (auto* v = obj.get(k)) {
            std::string s = trimStr(v->asString());
            if (!s.empty()) return s;
        }
    }
    return "";
}

struct PkgPiece {
    std::string url;
    uint64_t offset = 0;
    uint64_t size = 0;
};

inline bool extractPkgManifestFromJson(const std::string& jsonText, const std::string& baseUrl,
                                       std::string& outPkgUrl, std::vector<PkgPiece>& outPieces,
                                       std::string& outContentId, uint64_t& outSizeBytes,
                                       std::string& error) {
    outPkgUrl.clear();
    outPieces.clear();
    MiniJson::Value root;
    if (!MiniJson::parse(jsonText, root)) {
        error = "Manifesto JSON inválido";
        return false;
    }
    const MiniJson::Value* obj = &root;
    if (root.type == MiniJson::Value::Array && !root.arrVal.empty()) {
        obj = &root.arrVal[0];
    }
    if (obj->type != MiniJson::Value::Object) {
        error = "JSON sem objeto de pacote";
        return false;
    }

    std::string rawUrl = firstJsonString(*obj, {"pkg_url", "pkgUrl", "url", "pkg", "package", "download_url", "link"});
    uint64_t piecesSum = 0;

    if (rawUrl.empty()) {
        if (auto* piecesArr = obj->get("pieces")) {
            if (piecesArr->type == MiniJson::Value::Array) {
                uint64_t runningOffset = 0;
                for (const auto& entry : piecesArr->arrVal) {
                    if (entry.type != MiniJson::Value::Object) continue;
                    std::string u = firstJsonString(entry, {"url", "pkg_url", "pkgUrl", "download_url"});
                    std::string resolved = resolveServerUrl(baseUrl, u);
                    if (!isHttpUrl(resolved)) continue;
                    PkgPiece piece;
                    piece.url = resolved;
                    if (auto* fo = entry.get("fileOffset")) piece.offset = fo->asUInt(runningOffset);
                    else piece.offset = runningOffset;
                    if (auto* fs = entry.get("fileSize")) piece.size = fs->asUInt(0);
                    runningOffset = piece.offset + piece.size;
                    piecesSum += piece.size;
                    outPieces.push_back(std::move(piece));
                }
                if (!outPieces.empty()) {
                    std::sort(outPieces.begin(), outPieces.end(), [](const PkgPiece& a, const PkgPiece& b) {
                        return a.offset < b.offset;
                    });
                    outPkgUrl = outPieces[0].url;
                }
            }
        }
    }

    if (rawUrl.empty() && outPieces.empty()) {
        for (const char* arrKey : {"packages", "items", "assets"}) {
            if (auto* arr = obj->get(arrKey)) {
                if (arr->type == MiniJson::Value::Array) {
                    for (const auto& entry : arr->arrVal) {
                        if (entry.type != MiniJson::Value::Object) continue;
                        std::string candidate = firstJsonString(entry, {"pkg_url", "pkgUrl", "url", "browser_download_url", "download_url"});
                        if (!candidate.empty()) {
                            rawUrl = candidate;
                            if (auto* fs = entry.get("fileSize")) {
                                if (outSizeBytes == 0 && fs->asUInt() > 0) outSizeBytes = fs->asUInt();
                            }
                            if (lower(candidate).find(".pkg") != std::string::npos) break;
                        }
                    }
                    if (!rawUrl.empty()) break;
                }
            }
        }
    }
    if (rawUrl.empty() && outPieces.empty()) {
        if (auto* data = obj->get("data")) {
            if (data->type == MiniJson::Value::Object) {
                rawUrl = firstJsonString(*data, {"pkg_url", "pkgUrl", "url", "pkg", "download_url"});
            }
        }
    }

    if (outPkgUrl.empty()) {
        outPkgUrl = resolveServerUrl(baseUrl, rawUrl);
    }
    if (!isHttpUrl(outPkgUrl)) {
        error = "Campo 'url'/'pkg_url' ou 'pieces' ausente/inválido no JSON";
        return false;
    }
    std::string cid = firstJsonString(*obj, {"content_id", "contentId", "id"});
    if (validPkgContentId(cid)) outContentId = cid;
    if (auto* sz = obj->get("size_bytes")) {
        if (sz->asUInt() > 0) outSizeBytes = sz->asUInt();
    } else if (auto* szOrig = obj->get("originalFileSize")) {
        if (szOrig->asUInt() > 0) outSizeBytes = szOrig->asUInt();
    } else if (auto* sz2 = obj->get("size")) {
        if (sz2->type == MiniJson::Value::Number && sz2->asUInt() > 0) outSizeBytes = sz2->asUInt();
    }
    if (outSizeBytes == 0 && piecesSum > 0) outSizeBytes = piecesSum;
    return true;
}

inline bool extractPkgUrlFromJson(const std::string& jsonText, const std::string& baseUrl,
                                  std::string& outPkgUrl, std::string& outContentId,
                                  uint64_t& outSizeBytes, std::string& error) {
    std::vector<PkgPiece> pieces;
    return extractPkgManifestFromJson(jsonText, baseUrl, outPkgUrl, pieces, outContentId, outSizeBytes, error);
}

inline bool parseCatalogJson(const std::string& jsonText, const std::string& serverBase,
                             CatalogData& out, std::string& error) {
    MiniJson::Value root;
    if (!MiniJson::parse(jsonText, root)) {
        error = "Resposta do servidor não é um JSON válido";
        return false;
    }
    out = CatalogData{};

    const std::vector<MiniJson::Value>* itemsArr = nullptr;
    if (root.type == MiniJson::Value::Array) {
        itemsArr = &root.arrVal;
    } else if (root.type == MiniJson::Value::Object) {
        if (auto* v = root.get("store_name")) {
            if (!v->asString().empty()) out.storeName = v->asString();
        }
        if (auto* v = root.get("store_subtitle")) {
            if (!v->asString().empty()) out.storeSubtitle = v->asString();
        }
        if (auto* v = root.get("updated_at")) out.updatedAt = v->asInt();

        if (auto* cats = root.get("categories")) {
            if (cats->type == MiniJson::Value::Array) {
                for (const auto& c : cats->arrVal) {
                    if (c.type != MiniJson::Value::Object) continue;
                    Category cat;
                    if (auto* id = c.get("id")) cat.id = (int)id->asInt();
                    cat.name = firstJsonString(c, {"name", "title"});
                    if (auto* slug = c.get("slug")) cat.slug = slug->asString();
                    if (!cat.name.empty()) out.categories.push_back(cat);
                }
            }
        }

        for (const char* k : {"items", "homebrews", "packages", "apps", "games"}) {
            if (auto* arr = root.get(k)) {
                if (arr->type == MiniJson::Value::Array) {
                    itemsArr = &arr->arrVal;
                    break;
                }
            }
        }
        if (!itemsArr) {
            error = "Campo 'items' inválido no catálogo";
            return false;
        }
    } else {
        error = "Formato JSON de catálogo não suportado";
        return false;
    }

    int autoId = 1;
    for (const auto& it : *itemsArr) {
        if (it.type != MiniJson::Value::Object) continue;
        Game g;
        if (auto* v = it.get("id")) g.id = (int)v->asInt();
        if (g.id <= 0) g.id = autoId;
        ++autoId;

        g.title = firstJsonString(it, {"title", "name"});
        g.category = firstJsonString(it, {"category", "genre", "type"});
        if (auto* v = it.get("category_id")) g.categoryId = (int)v->asInt();
        g.version = firstJsonString(it, {"version", "ver"});
        g.author = firstJsonString(it, {"author", "developer", "publisher"});
        g.description = firstJsonString(it, {"description", "desc"});
        if (auto* v = it.get("size_bytes")) g.sizeBytes = v->asUInt();
        g.size = firstJsonString(it, {"size", "size_text"});
        if (g.size.empty() && g.sizeBytes > 0) g.size = bytesText(g.sizeBytes);
        g.contentId = firstJsonString(it, {"content_id", "contentId"});
        if (auto* v = it.get("content_type")) g.contentType = (uint32_t)v->asUInt(0x1a);
        if (auto* v = it.get("content_flags")) g.contentFlags = (uint32_t)v->asUInt(0);
        g.url = resolveServerUrl(serverBase, firstJsonString(it, {"pkg_url", "pkgUrl", "url", "pkg", "download_url"}));
        g.image = resolveServerUrl(serverBase, firstJsonString(it, {"cover_url", "coverUrl", "cover", "image", "image_url", "icon"}));
        if (auto* v = it.get("updated_at")) g.updatedAt = v->asInt();

        if (g.category.empty()) g.category = "Geral";
        if (g.id > 0 && !g.title.empty() && isHttpUrl(g.url)) {
            out.items.push_back(std::move(g));
        }
    }

    if (out.categories.empty() && !out.items.empty()) {
        std::set<std::string> seen;
        int cid = 1;
        for (const auto& item : out.items) {
            if (seen.insert(lower(item.category)).second) {
                Category cat;
                cat.id = cid++;
                cat.name = item.category;
                out.categories.push_back(cat);
            }
        }
    }
    return true;
}

inline std::string serializeCategories(const std::vector<Category>& cats) {
    std::string s;
    for (const auto& c : cats) {
        s += std::to_string(c.id) + "\t" + cleanField(c.name) + "\t" + cleanField(c.slug) + "\n";
    }
    return s;
}

inline std::vector<Category> loadCategories(const std::string& raw) {
    std::vector<Category> out;
    size_t p = 0;
    while (p < raw.size()) {
        auto e = raw.find('\n', p);
        if (e == std::string::npos) e = raw.size();
        std::string line = raw.substr(p, e - p);
        p = e + 1;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        Category c;
        c.id = atoi(line.substr(0, t1).c_str());
        c.name = t2 == std::string::npos ? line.substr(t1 + 1) : line.substr(t1 + 1, t2 - t1 - 1);
        c.slug = t2 == std::string::npos ? "" : line.substr(t2 + 1);
        if (!c.name.empty()) out.push_back(c);
    }
    return out;
}

inline std::string serialize(const std::vector<Game>& v) {
    std::string s;
    for (const auto& g : v) {
        s += std::to_string(g.id);
        s += "\t" + cleanField(g.title);
        s += "\t" + cleanField(g.category);
        s += "\t" + std::to_string(g.categoryId);
        s += "\t" + cleanField(g.version);
        s += "\t" + cleanField(g.author);
        s += "\t" + cleanField(g.description);
        s += "\t" + cleanField(g.size);
        s += "\t" + std::to_string((unsigned long long)g.sizeBytes);
        s += "\t" + cleanField(g.contentId);
        s += "\t" + std::to_string(g.contentType);
        s += "\t" + std::to_string(g.contentFlags);
        s += "\t" + cleanField(g.url);
        s += "\t" + cleanField(g.image);
        s += "\t" + std::to_string(g.updatedAt);
        s += "\n";
    }
    return s;
}

inline std::vector<Game> loadCatalog(const std::string& s) {
    std::vector<Game> v;
    size_t p = 0;
    while (p < s.size()) {
        auto e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(p, e - p);
        p = e + 1;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) {
            auto b = line.find('\t', a);
            f.push_back(line.substr(a, b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() != 15) continue;
        Game g;
        g.id = atoi(f[0].c_str());
        g.title = f[1];
        g.category = f[2].empty() ? "Geral" : f[2];
        g.categoryId = atoi(f[3].c_str());
        g.version = f[4];
        g.author = f[5];
        g.description = f[6];
        g.size = f[7];
        g.sizeBytes = strtoull(f[8].c_str(), nullptr, 10);
        g.contentId = f[9];
        g.contentType = (uint32_t)strtoul(f[10].c_str(), nullptr, 10);
        if (!g.contentType) g.contentType = 0x1a;
        g.contentFlags = (uint32_t)strtoul(f[11].c_str(), nullptr, 10);
        g.url = f[12];
        g.image = f[13];
        g.updatedAt = strtoll(f[14].c_str(), nullptr, 10);
        if (g.id > 0 && !g.title.empty() && isHttpUrl(g.url)) {
            v.push_back(std::move(g));
        }
    }
    return v;
}
