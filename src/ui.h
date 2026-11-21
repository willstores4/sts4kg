#pragma once
#include "catalog.h"
#include "data_root.h"
#include "installer.h"
#include <cmath>
#include <cstring>
#include <stdint.h>
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

struct Bitmap {
    int w = 0, h = 0;
    std::vector<unsigned char> p, a;
};

struct Glyph {
    int w = 0, h = 0, x = 0, y = 0, advance = 0;
    std::vector<unsigned char> b;
};

struct Canvas {
    static const int W = 1920, H = 1080;
    std::vector<uint32_t> pixels;
    std::vector<unsigned char> font;
    stbtt_fontinfo fi;
    std::map<int, Glyph> glyphs;

    Canvas(const std::string& base) : pixels(W * H) {
        auto s = readFile(base + "/font.ttf");
        font.assign(s.begin(), s.end());
        if (font.empty() || !stbtt_InitFont(&fi, font.data(), 0)) {
            fprintf(stderr, "Font missing\n");
            exit(1);
        }
    }

    void rect(int x, int y, int w, int h, uint32_t c) {
        int r = std::min(W, x + w), b = std::min(H, y + h);
        for (int j = std::max(0, y); j < b; ++j) {
            std::fill(pixels.begin() + j * W + std::max(0, x),
                      pixels.begin() + j * W + std::max(std::max(0, x), r),
                      0xff000000 | c);
        }
    }

    void blend(int x, int y, uint32_t c, int a) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        auto& d = pixels[y * W + x];
        int r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
        int g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
        int b = ((c & 255) * a + (d & 255) * (255 - a)) / 255;
        d = 0xff000000 | (r << 16) | (g << 8) | b;
    }

    void line(int x, int y, int xx, int yy, uint32_t color) {
        int steps = std::max(abs(xx - x), abs(yy - y));
        for (int i = 0; i <= steps; i++) {
            blend(x + (xx - x) * i / std::max(1, steps), y + (yy - y) * i / std::max(1, steps), color, 255);
        }
    }

    void rounded(int x, int y, int w, int h, int radius, uint32_t color) {
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int dx = i < radius ? radius - i : i >= w - radius ? i - (w - radius - 1) : 0;
                int dy = j < radius ? radius - j : j >= h - radius ? j - (h - radius - 1) : 0;
                if (dx * dx + dy * dy <= radius * radius) blend(x + i, y + j, color, 255);
            }
        }
    }

    void region(const Bitmap& b, int sx, int sy, int sw, int sh, int x, int y, int w, int h) {
        if (b.p.empty() || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int dx = x + i, dy = y + j;
                if (dx < 0 || dy < 0 || dx >= W || dy >= H) continue;
                float fx = std::max(0.f, std::min(float(b.w - 1), sx + (i + .5f) * sw / w - .5f));
                float fy = std::max(0.f, std::min(float(b.h - 1), sy + (j + .5f) * sh / h - .5f));
                int x0 = int(fx), y0 = int(fy), x1 = std::min(b.w - 1, x0 + 1), y1 = std::min(b.h - 1, y0 + 1);
                float tx = fx - x0, ty = fy - y0;
                const unsigned char* p00 = &b.p[(y0 * b.w + x0) * 3];
                const unsigned char* p10 = &b.p[(y0 * b.w + x1) * 3];
                const unsigned char* p01 = &b.p[(y1 * b.w + x0) * 3];
                const unsigned char* p11 = &b.p[(y1 * b.w + x1) * 3];
                int rgb[3];
                for (int k = 0; k < 3; k++) {
                    float top = p00[k] * (1 - tx) + p10[k] * tx;
                    float bottom = p01[k] * (1 - tx) + p11[k] * tx;
                    rgb[k] = int(top * (1 - ty) + bottom * ty + .5f);
                }
                uint32_t color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
                if (b.a.empty()) {
                    pixels[dy * W + dx] = 0xff000000 | color;
                } else {
                    float top = b.a[y0 * b.w + x0] * (1 - tx) + b.a[y0 * b.w + x1] * tx;
                    float bottom = b.a[y1 * b.w + x0] * (1 - tx) + b.a[y1 * b.w + x1] * tx;
                    blend(dx, dy, color, int(top * (1 - ty) + bottom * ty + .5f));
                }
            }
        }
    }

    Glyph& glyph(unsigned cp, int size) {
        int key = cp * 128 + size;
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        Glyph g;
        float scale = stbtt_ScaleForPixelHeight(&fi, (float)size);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb);
        g.advance = int(adv * scale);
        auto b = stbtt_GetCodepointBitmap(&fi, 0, scale, cp, &g.w, &g.h, &g.x, &g.y);
        if (b) {
            g.b.assign(b, b + g.w * g.h);
            stbtt_FreeBitmap(b, nullptr);
        }
        return glyphs.emplace(key, std::move(g)).first->second;
    }

    int text(const std::string& s, int x, int y, int size, uint32_t color, int maxW = 1800) {
        int start = x;
        for (size_t i = 0; i < s.size();) {
            unsigned cp = (unsigned char)s[i++];
            if (cp >= 192) {
                int n = cp < 224 ? 1 : cp < 240 ? 2 : 3;
                cp &= n == 1 ? 31 : n == 2 ? 15 : 7;
                while (n-- && i < s.size()) cp = (cp << 6) | ((unsigned char)s[i++] & 63);
            }
            auto& g = glyph(cp, size);
            if (x + g.advance > start + maxW) break;
            for (int j = 0; j < g.h; ++j) {
                for (int k = 0; k < g.w; ++k) {
                    if (g.b[j * g.w + k]) blend(x + k + g.x, y + j + g.y, color, g.b[j * g.w + k]);
                }
            }
            x += g.advance;
        }
        return x - start;
    }

    void lines(std::string s, int x, int y, int size, uint32_t c, int width, int count) {
        for (int n = 0; n < count && !s.empty(); ++n) {
            int used = 0;
            size_t end = 0, last = 0;
            while (end < s.size()) {
                unsigned cp = (unsigned char)s[end];
                size_t step = cp < 128 ? 1 : cp < 224 ? 2 : cp < 240 ? 3 : 4;
                unsigned code = cp;
                if (step > 1) {
                    code &= step == 2 ? 31 : step == 3 ? 15 : 7;
                    for (size_t z = 1; z < step && end + z < s.size(); ++z) {
                        code = (code << 6) | ((unsigned char)s[end + z] & 63);
                    }
                }
                if (used + glyph(code, size).advance > width) break;
                used += glyph(code, size).advance;
                if (s[end] == ' ') last = end;
                end += step;
            }
            if (!end) break;
            if (end < s.size() && last) end = last;
            text(s.substr(0, end), x, y + n * (size + 5), size, c, width);
            s = s.substr(end);
            while (!s.empty() && s[0] == ' ') s.erase(0, 1);
        }
    }

    int textWidth(const std::string& s, int size) {
        int w = 0;
        for (size_t i = 0; i < s.size();) {
            unsigned cp = (unsigned char)s[i++];
            if (cp >= 192) {
                int n = cp < 224 ? 1 : cp < 240 ? 2 : 3;
                cp &= n == 1 ? 31 : n == 2 ? 15 : 7;
                while (n-- && i < s.size()) cp = (cp << 6) | ((unsigned char)s[i++] & 63);
            }
            w += glyph(cp, size).advance;
        }
        return w;
    }

    void ring(int cx, int cy, int radius, int thickness, uint32_t color, int alpha = 255) {
        if (radius <= 1 || thickness <= 0 || alpha <= 0) return;
        float rOuter = float(radius);
        float rInner = std::max(0.0f, rOuter - float(thickness));
        int bound = radius + 2;
        int x0 = std::max(0, cx - bound), x1 = std::min(W - 1, cx + bound);
        int y0 = std::max(0, cy - bound), y1 = std::min(H - 1, cy + bound);
        for (int py = y0; py <= y1; ++py) {
            float dy = float(py - cy);
            for (int px = x0; px <= x1; ++px) {
                float dx = float(px - cx);
                float d = std::sqrt(dx * dx + dy * dy);
                if (d < rInner - 1.0f || d > rOuter + 1.0f) continue;
                float cov = 1.0f;
                if (d > rOuter) cov = std::max(0.0f, 1.0f - (d - rOuter));
                else if (d < rInner) cov = std::max(0.0f, 1.0f - (rInner - d));
                int a = int(cov * alpha + 0.5f);
                if (a > 0) blend(px, py, color, a);
            }
        }
    }

    void spinner(int cx, int cy, int radius, int thickness, float angleRad,
                 uint32_t trackColor, uint32_t arcColor, float sweepRad = 2.75f) {
        if (radius <= 1 || thickness <= 0) return;
        const float kTwoPi = 6.2831853f;
        float rOuter = float(radius);
        float rInner = std::max(0.0f, rOuter - float(thickness));
        float normBase = std::fmod(angleRad, kTwoPi);
        if (normBase < 0.0f) normBase += kTwoPi;

        int bound = radius + 2;
        int x0 = std::max(0, cx - bound), x1 = std::min(W - 1, cx + bound);
        int y0 = std::max(0, cy - bound), y1 = std::min(H - 1, cy + bound);
        for (int py = y0; py <= y1; ++py) {
            float dy = float(py - cy);
            for (int px = x0; px <= x1; ++px) {
                float dx = float(px - cx);
                float d = std::sqrt(dx * dx + dy * dy);
                if (d < rInner - 1.0f || d > rOuter + 1.0f) continue;
                float cov = 1.0f;
                if (d > rOuter) cov = std::max(0.0f, 1.0f - (d - rOuter));
                else if (d < rInner) cov = std::max(0.0f, 1.0f - (rInner - d));
                int trackA = int(cov * 115.0f + 0.5f);
                if (trackA > 0) blend(px, py, trackColor, trackA);

                float a = std::atan2(dy, dx);
                if (a < 0.0f) a += kTwoPi;
                float rel = a - normBase;
                if (rel < 0.0f) rel += kTwoPi;
                if (rel <= sweepRad) {
                    float t = rel / std::max(0.01f, sweepRad);
                    int arcA = int(cov * (90.0f + 165.0f * t) + 0.5f);
                    if (arcA > 0) blend(px, py, arcColor, arcA);
                }
            }
        }
    }

    void image(const Bitmap& b, int x, int y, int w, int h) {
        if (b.p.empty()) return;
        float scale = std::max(float(w) / b.w, float(h) / b.h);
        int sw = std::max(1, int(w / scale)), sh = std::max(1, int(h / scale));
        region(b, (b.w - sw) / 2, (b.h - sh) / 2, sw, sh, x, y, w, h);
    }
};

inline Bitmap loadBitmap(const std::string& p) {
    Bitmap b;
    int c = 0;
    unsigned char* d = stbi_load(p.c_str(), &b.w, &b.h, &c, 3);
    if (d) {
        b.p.assign(d, d + size_t(b.w) * b.h * 3);
        stbi_image_free(d);
    }
    return b;
}

inline Bitmap loadBitmapRGBA(const std::string& p) {
    Bitmap b;
    int c = 0;
    unsigned char* d = stbi_load(p.c_str(), &b.w, &b.h, &c, 4);
    if (d) {
        b.p.resize(size_t(b.w) * b.h * 3);
        b.a.resize(size_t(b.w) * b.h);
        for (size_t i = 0; i < b.a.size(); ++i) {
            b.p[i * 3] = d[i * 4];
            b.p[i * 3 + 1] = d[i * 4 + 1];
            b.p[i * 3 + 2] = d[i * 4 + 2];
            b.a[i] = d[i * 4 + 3];
        }
        stbi_image_free(d);
    }
    return b;
}

inline Bitmap loadBlueCase(const std::string& p) {
    return loadBitmapRGBA(p);
}

enum Action {
    UP, DOWN, LEFT, RIGHT, CONFIRM, BACK, SEARCH, FAVORITE,
    TAB_PREV, TAB_NEXT, SORT, REFRESH, PAGE_PREV, PAGE_NEXT,
    DOWNLOADS, SERVER_CONFIG, LOCAL_INSTALL, BINLOADER_INSTALL, REMOVE_TASK
};

struct UI {
    enum KeyboardTarget { KbNone = 0, KbSearch = 1, KbServer = 2 };

    Canvas c;
    std::string base, data;
    std::string storeName = "HB_Store";
    std::string storeSubtitle = "HOMEBREW PS4";
    std::string serverUrl;
    std::string status = "Pronto para sincronizar (OPTIONS)";
    std::string query;
    Bitmap logo, caseFrame;

    std::vector<Category> categories;
    std::vector<Game> games;
    std::vector<int> visible;
    std::set<int> favorites;
    std::map<int, Bitmap> covers;

    int selected = 0, tab = 0, sort = 0, key = 0, detailLink = 0;
    bool detail = false, keyboard = false, dirty = true, syncing = false;
    int animFrame = 0;
    KeyboardTarget kbTarget = KbNone;
    std::string kbBuffer;
    std::string notice;

    // Fila de instalações / Notificações PS4
    std::vector<InstallPackage> tasks;
    int taskSelected = 0;
    bool installBgRequest = false, installLocalRequest = false, installBinLoaderRequest = false;
    Game installItem;
    std::vector<Game> installBatch;

    // Ações na aba Notificações
    bool taskToggleRequest = false, taskRetryRequest = false, taskCancelRequest = false, taskRemoveRequest = false;
    int actionTaskId = 0;

    // Configurar Servidor
    bool serverPicker = false;
    int serverChoice = 0;
    bool serverEditRequest = false, serverSyncRequest = false, serverResetRequest = false;
    std::string newServerUrl;
    bool serverSaveRequest = false;

    // Exclusão de arquivos em caminhos pré-selecionados e conciliação de status
    std::string detailDirectPath, detailDirectDir;
    bool detailDirectExists = false;
    uint64_t detailDirectSize = 0;
    bool detailProbeRequest = false;

    bool deleteConfirm = false;
    std::string deleteTargetPath;
    uint64_t deleteTargetSize = 0;
    int deleteTargetTaskId = 0;
    bool deleteExecuteRequest = false;

    bool filesModal = false;
    std::vector<std::string> filesDirs;
    int filesDirIndex = 0;
    std::vector<PortalFileEntry> filesEntries;
    int fileSelected = 0;
    std::string filesError;
    bool filesRefreshRequest = false;
    bool reconcileStatusRequest = false;

    int downloadsTab() const { return 2 + int(categories.size()); }
    int totalTabs() const { return 3 + int(categories.size()); }
    int selectedTaskId() const {
        if (tasks.empty()) return 0;
        return tasks[std::min(taskSelected, int(tasks.size()) - 1)].id;
    }

    float spinnerAngle() const { return float(animFrame) * 0.105f; }

    static bool isTaskActive(const InstallPackage& t) {
        return t.state == InstallState::Queued ||
               t.state == InstallState::Downloading ||
               t.state == InstallState::Registering ||
               t.state == InstallState::Registered ||
               t.state == InstallState::Installing;
    }

    static int taskDisplayPercent(const InstallPackage& t) {
        if (t.state == InstallState::Complete) return 100;
        if (t.percent >= 0) return std::min(100, t.percent);
        if (t.bytes > 0) return (int)std::min<uint64_t>(99, t.doneBytes * 100 / t.bytes);
        if (t.state == InstallState::Queued || t.state == InstallState::Registering ||
            t.state == InstallState::Registered ||
            (t.state == InstallState::Downloading && t.doneBytes == 0)) return 0;
        return -1;
    }

    static std::string formatPercentText(int pct) {
        return pct >= 0 ? (std::to_string(pct) + "%") : "--%";
    }

    const InstallPackage* findDirectTaskForItem(int itemId, const std::string& urlFilter = "") const {
        const InstallPackage* fallback = nullptr;
        for (const auto& t : tasks) {
            if (t.mode != InstallMode::DirectSave) continue;
            bool matchId = (itemId > 0 && t.itemId == itemId);
            bool matchUrl = (!urlFilter.empty() && t.url == urlFilter);
            if (!urlFilter.empty()) {
                if (!(matchUrl && (itemId <= 0 || matchId || t.itemId == 0))) continue;
            } else {
                if (!matchId) continue;
            }
            if (isTaskActive(t)) return &t;
            if (!fallback) fallback = &t;
        }
        return fallback;
    }

    int countActiveDirectTasks(int itemId = 0, int* outDone = nullptr, int* outTotal = nullptr) const {
        int active = 0, doneCount = 0, total = 0;
        for (const auto& t : tasks) {
            if (t.mode != InstallMode::DirectSave) continue;
            if (itemId > 0 && t.itemId != itemId) continue;
            ++total;
            if (isTaskActive(t)) ++active;
            else if (t.state == InstallState::Complete) ++doneCount;
        }
        if (outDone) *outDone = doneCount;
        if (outTotal) *outTotal = total;
        return active;
    }

    const InstallPackage* primaryActiveDirectTask() const {
        for (const auto& t : tasks) {
            if (t.mode == InstallMode::DirectSave && isTaskActive(t)) return &t;
        }
        return nullptr;
    }

    bool hasSpinnerActivity() const {
        return syncing || primaryActiveDirectTask() != nullptr;
    }

    void drawProgressBar(int x, int y, int w, int h, int pct, bool active, uint32_t trackColor, uint32_t fillColor) {
        c.rect(x, y, w, h, trackColor);
        if (pct >= 0) {
            int fw = w * std::min(100, pct) / 100;
            if (fw > 0) c.rect(x, y, fw, h, fillColor);
        } else if (active) {
            int segW = std::max(36, w / 4);
            int rawPos = (animFrame * 8) % std::max(1, w + segW);
            int bx = x + rawPos - segW;
            int clippedStart = std::max(x, bx);
            int clippedEnd = std::min(x + w, bx + segW);
            if (clippedEnd > clippedStart) {
                c.rect(clippedStart, y, clippedEnd - clippedStart, h, fillColor);
            }
        }
    }

    void drawActivityIcon(const InstallPackage& t, int cx, int cy, int radius, int thickness, bool onBrightBg = false) {
        uint32_t trackCol = onBrightBg ? 0x16538f : 0x163154;
        uint32_t arcCol = onBrightBg ? 0x060b12 : 0x5cb8ff;
        uint32_t okCol = onBrightBg ? 0x060b12 : 0x34d399;
        uint32_t pauseCol = onBrightBg ? 0x060b12 : 0xfbbf24;
        uint32_t errCol = onBrightBg ? 0x060b12 : 0xff6b6b;

        if (isTaskActive(t)) {
            c.spinner(cx, cy, radius, thickness, spinnerAngle(), trackCol, arcCol);
        } else if (t.state == InstallState::Complete) {
            c.ring(cx, cy, radius, thickness, okCol, 255);
            if (radius >= 16) {
                int fs = std::max(12, radius * 58 / 100);
                int tw = c.textWidth("OK", fs);
                c.text("OK", cx - tw / 2, cy + (fs + 2) / 3, fs, okCol);
            }
        } else if (t.state == InstallState::Paused) {
            c.ring(cx, cy, radius, thickness, pauseCol, 220);
            if (radius >= 14) {
                int bh = std::max(8, radius * 75 / 100);
                int bw = std::max(2, thickness - 1);
                c.rect(cx - bw - 2, cy - bh / 2, bw, bh, pauseCol);
                c.rect(cx + 2, cy - bh / 2, bw, bh, pauseCol);
            }
        } else {
            c.ring(cx, cy, radius, thickness, errCol, 230);
            if (radius >= 14) {
                int fs = std::max(14, radius * 85 / 100);
                int tw = c.textWidth("!", fs);
                c.text("!", cx - tw / 2, cy + (fs + 2) / 3, fs, errCol);
            }
        }
    }

    void drawHeaderActivityPill(int dlTab) {
        const InstallPackage* act = primaryActiveDirectTask();
        if (!act && !syncing) return;
        const uint32_t cyan = 0x318ee0, white = 0xe7eff7;
        int px = (tab == dlTab) ? 1390 : 1120;
        int pw = (tab == dlTab) ? 508 : 486;
        c.rounded(px, 25, pw, 42, 6, 0x0d213a);
        c.rect(px, 25, 3, 42, cyan);
        c.spinner(px + 25, 46, 12, 3, spinnerAngle(), 0x1a365d, 0x5cb8ff);
        if (act) {
            std::string pctStr = formatPercentText(taskDisplayPercent(*act));
            int pctW = c.textWidth(pctStr, 20);
            c.text(pctStr, px + pw - 16 - pctW, 53, 20, 0x5cb8ff, pctW + 8);
            int nAct = countActiveDirectTasks();
            std::string prefix = nAct > 1 ? ("(" + std::to_string(nAct) + ") ") : "";
            std::string label = prefix + act->name;
            int maxLbl = std::max(80, pw - 74 - pctW);
            c.text(label, px + 46, 52, 18, white, maxLbl);
        } else {
            c.text("Sincronizando catálogo...", px + 46, 52, 18, white, pw - 62);
        }
    }

    void drawDetailProgressWidget(const Game& g, const Game& shown, bool isAll) {
        const InstallPackage* t = isAll ? findDirectTaskForItem(g.id, "") : findDirectTaskForItem(g.id, shown.url);
        if (!t && isAll) return;
        if (!t && g.links.size() <= 1 && shown.directDownload) t = findDirectTaskForItem(g.id, "");
        if (!t || t->mode != InstallMode::DirectSave) return;

        const uint32_t cyan = 0x318ee0, white = 0xe7eff7, muted = 0xb2bec9;
        bool active = isTaskActive(*t);
        uint32_t accentCol = active ? cyan
                           : (t->state == InstallState::Complete) ? uint32_t(0x34d399)
                           : (t->state == InstallState::Paused)   ? uint32_t(0xfbbf24)
                                                                  : uint32_t(0xff6b6b);

        c.rounded(382, 756, 390, 156, 8, 0x091a32);
        c.rect(382, 756, 5, 156, accentCol);

        drawActivityIcon(*t, 428, 804, 24, 5, false);

        std::string phaseLabel = active ? std::string(installStateName(t->state)) + "..." : installStateLabel(*t);
        if (isAll) {
            int doneCnt = 0, totCnt = 0;
            int actCnt = countActiveDirectTasks(g.id, &doneCnt, &totCnt);
            if (totCnt > 1) {
                phaseLabel += " (" + std::to_string(doneCnt) + "/" + std::to_string(totCnt) +
                              (actCnt > 0 ? ", " + std::to_string(actCnt) + " at." : "") + ")";
            }
        }
        c.text(phaseLabel, 468, 792, 19, white, 290);

        int pct = taskDisplayPercent(*t);
        std::string pctStr = formatPercentText(pct);
        int pctW = c.textWidth(pctStr, 32);
        c.text(pctStr, 468, 828, 32, accentCol, 120);

        std::string bytesStr;
        if (t->bytes > 0) bytesStr = bytesText(t->doneBytes) + " / " + bytesText(t->bytes);
        else if (t->doneBytes > 0) bytesStr = bytesText(t->doneBytes) + " baixados";
        else bytesStr = active ? "Aguardando..." : "";
        int bytesX = 468 + pctW + 12;
        if (!bytesStr.empty() && bytesX < 756) {
            c.text(bytesStr, bytesX, 825, 17, white, 758 - bytesX);
        }

        drawProgressBar(400, 848, 354, 8, pct, active, 0x050b16, accentCol);

        std::string infoLine = !t->path.empty() ? ("Destino: " + t->path) : (t->note.empty() ? installStatusText(*t) : t->note);
        c.text(infoLine, 400, 879, 16, t->state == InstallState::Error ? 0xffa5a5 : muted, 354);

        std::string footLine = active ? "CÍRCULO fecha janela (continua baixando)" : installStatusText(*t);
        c.text(footLine, 400, 903, 15, t->state == InstallState::Error ? 0xffa5a5 : cyan, 354);
    }

    std::string tabTitle(int idx) const {
        if (idx == 0) return "Todos";
        if (idx == 1) return "Favoritos";
        if (idx >= 2 && idx < 2 + int(categories.size())) return categories[idx - 2].name;
        return "Notificações";
    }

    int countForCategory(const Category& cat) const {
        int n = 0;
        for (const auto& g : games) {
            if ((cat.id > 0 && g.categoryId == cat.id) || lower(g.category) == lower(cat.name)) ++n;
        }
        return n;
    }

    UI(std::string b, std::string d)
        : c(b), base(b), data(d),
          logo(loadBitmapRGBA(base + "/brand/nullstore-bag.png")),
          caseFrame(loadBlueCase(base + "/brand/ps4-case-frame.png")) {
        serverUrl = readDefaultServerUrl(base, data);
        categories = loadCategories(readFile(data + "/categories.tsv"));
        games = loadCatalog(readFile(data + "/catalog.tsv"));
        std::string savedName = trimStr(readFile(data + "/store-name.txt"));
        if (!savedName.empty()) storeName = savedName;

        std::string fav = readFile(data + "/favorites.txt");
        size_t p = 0;
        while (p < fav.size()) {
            int id = atoi(fav.c_str() + p);
            if (id > 0) favorites.insert(id);
            auto e = fav.find('\n', p);
            if (e == std::string::npos) break;
            p = e + 1;
        }
        filter();
    }

    void filter() {
        visible.clear();
        int dlTab = downloadsTab();
        if (tab > dlTab) tab = 0;

        for (size_t i = 0; i < games.size(); ++i) {
            const auto& g = games[i];
            if (tab == 1 && !favorites.count(g.id)) continue;
            if (tab >= 2 && tab < dlTab) {
                const auto& cat = categories[tab - 2];
                bool matchCat = (cat.id > 0 && g.categoryId == cat.id) || lower(g.category) == lower(cat.name);
                if (!matchCat) continue;
            }
            if (!query.empty()) {
                std::string hay = lower(g.title + " " + g.author + " " + g.category + " " + g.contentId);
                if (hay.find(lower(query)) == std::string::npos) continue;
            }
            visible.push_back((int)i);
        }

        std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) {
            const auto& x = games[a];
            const auto& y = games[b];
            if (sort == 1) return lower(x.title) < lower(y.title);
            if (sort == 2) return sizeMB(x.size) < sizeMB(y.size);
            if (sort == 3) return sizeMB(x.size) > sizeMB(y.size);
            return x.id > y.id;
        });

        selected = visible.empty() ? 0 : std::max(0, std::min(selected, int(visible.size()) - 1));
        dirty = true;
    }

    Game* current() {
        return visible.empty() ? nullptr : &games[visible[selected]];
    }

    Bitmap& cover(const Game& g) {
        auto it = covers.find(g.id);
        if (it != covers.end()) return it->second;
        if (covers.size() >= 24) covers.clear();
        auto b = loadBitmap(data + "/covers/" + std::to_string(g.id) + ".jpg");
        return covers.emplace(g.id, std::move(b)).first->second;
    }

    void favorite() {
        auto* g = current();
        if (!g) return;
        if (favorites.count(g->id)) favorites.erase(g->id);
        else favorites.insert(g->id);
        std::string s;
        for (int id : favorites) s += std::to_string(id) + "\n";
        if (!writeFile(data + "/favorites.txt", s)) {
            notice = "Não foi possível salvar favoritos.";
        }
        filter();
    }

    int findTaskIdForPath(const std::string& filePath) const {
        if (filePath.empty()) return 0;
        for (const auto& t : tasks) {
            if (t.mode == InstallMode::DirectSave && t.path == filePath) return t.id;
        }
        return 0;
    }

    bool isPathDownloading(const std::string& filePath) const {
        if (filePath.empty()) return false;
        for (const auto& t : tasks) {
            if (t.mode == InstallMode::DirectSave && t.path == filePath &&
                (t.state == InstallState::Queued || t.state == InstallState::Downloading)) {
                return true;
            }
        }
        return false;
    }

    static std::string normalizeBrowserDir(const std::string& raw) {
        if (raw.empty()) return "";
        bool trailing = false;
        std::string err;
        std::string clean = normalizePs4PathInput(raw, trailing, err);
        if (clean.empty()) return "";
        const std::string storeRoot = hbStoreDataRoot();
        if (clean == storeRoot || clean.compare(0, storeRoot.size() + 1, storeRoot + "/") == 0) {
            return "";
        }
        return clean;
    }

    void refreshDetailFileProbe() {
        detailDirectPath.clear();
        detailDirectDir.clear();
        detailDirectExists = false;
        detailDirectSize = 0;
        if (!detail || !current()) return;

        const auto& g = *current();
        int nLinks = !g.links.empty() ? int(g.links.size()) : 1;
        int totalChoices = (nLinks > 1) ? (nLinks + 1) : 1;
        int selLink = std::max(0, std::min(detailLink, totalChoices - 1));
        bool isAll = (nLinks > 1 && selLink == nLinks);
        if (isAll) return;

        Game shown = gameForLink(g, selLink);
        if (!shown.directDownload) return;

        std::string rawDest = shown.directPath.empty() ? std::string("/data/pkg") : shown.directPath;
        std::string err;
        bool explicitFile = false;
        if (!resolveDirectDownloadTarget(rawDest, shown.url, shown.title, false,
                                         detailDirectDir, detailDirectPath, explicitFile, err, true)) {
            detailDirectDir = "/data/pkg";
            detailDirectPath.clear();
        }
        if (const InstallPackage* lt = findDirectTaskForItem(g.id, shown.url)) {
            if (!lt->path.empty()) {
                detailDirectPath = lt->path;
                size_t sl = lt->path.rfind('/');
                if (sl != std::string::npos) detailDirectDir = (sl == 0) ? "/" : lt->path.substr(0, sl);
            }
        }
        uint64_t sz = 0;
        if (!detailDirectPath.empty() && portalRegularFileStatus(detailDirectPath, sz) == 1) {
            detailDirectExists = true;
            detailDirectSize = sz;
        }
    }

    void collectConfiguredDirectDirs(const std::string& preferredDir = "") {
        filesDirs.clear();
        auto addDir = [&](const std::string& raw, bool asDir) {
            if (raw.empty()) return;
            std::string candidate = raw;
            if (!asDir) {
                size_t sl = candidate.rfind('/');
                if (sl != std::string::npos && sl > 0) candidate = candidate.substr(0, sl);
            } else {
                bool trailing = false;
                std::string err;
                std::string clean = normalizePs4PathInput(candidate, trailing, err);
                if (!clean.empty() && !trailing) {
                    size_t sl = clean.rfind('/');
                    std::string seg = (sl == std::string::npos) ? clean : clean.substr(sl + 1);
                    size_t dot = seg.rfind('.');
                    if (dot != std::string::npos && dot > 0 && sl > 0) {
                        candidate = clean.substr(0, sl);
                    }
                }
            }
            std::string norm = normalizeBrowserDir(candidate);
            if (norm.empty()) return;
            if (std::find(filesDirs.begin(), filesDirs.end(), norm) == filesDirs.end()) {
                filesDirs.push_back(norm);
            }
        };

        addDir(preferredDir, true);
        if (const Game* cur = current()) {
            addDir(cur->directPath, true);
            for (const auto& lk : cur->links) addDir(lk.directPath, true);
        }
        addDir("/data/pkg", true);
        addDir("/data", true);
        for (const auto& g : games) {
            if (g.directDownload) addDir(g.directPath, true);
            for (const auto& lk : g.links) {
                if (lk.directDownload) addDir(lk.directPath, true);
            }
        }
        for (const auto& t : tasks) {
            if (t.mode == InstallMode::DirectSave && !t.path.empty()) addDir(t.path, false);
        }
        if (filesDirs.empty()) filesDirs.push_back("/data");
    }

    void refreshFilesListing() {
        filesEntries.clear();
        filesError.clear();
        if (filesDirs.empty()) collectConfiguredDirectDirs();
        filesDirIndex = std::max(0, std::min(filesDirIndex, int(filesDirs.size()) - 1));
        portalListRegularFiles(filesDirs[filesDirIndex], filesEntries, filesError);
        fileSelected = filesEntries.empty() ? 0 : std::max(0, std::min(fileSelected, int(filesEntries.size()) - 1));
    }

    void openFilesManager(const std::string& preferredDir = "") {
        collectConfiguredDirectDirs(preferredDir);
        filesDirIndex = 0;
        fileSelected = 0;
        filesModal = true;
        refreshFilesListing();
        reconcileStatusRequest = true;
    }

    void openDeleteConfirmation(const std::string& filePath, uint64_t sizeBytes, int taskId = 0) {
        if (filePath.empty()) return;
        if (isPathDownloading(filePath)) {
            notice = "Download em andamento; cancele antes de excluir.";
            return;
        }
        deleteTargetPath = filePath;
        deleteTargetSize = sizeBytes;
        deleteTargetTaskId = taskId > 0 ? taskId : findTaskIdForPath(filePath);
        deleteConfirm = true;
    }

    void action(Action a) {
        dirty = true;
        notice.clear();
        int dlTab = downloadsTab();

        if (deleteConfirm) {
            if (a == BACK) { deleteConfirm = false; return; }
            if (a == CONFIRM) {
                deleteConfirm = false;
                deleteExecuteRequest = true;
                return;
            }
            return;
        }

        if (filesModal) {
            if (a == BACK) { filesModal = false; return; }
            if (!filesDirs.empty() && a == TAB_PREV) {
                filesDirIndex = (filesDirIndex + int(filesDirs.size()) - 1) % int(filesDirs.size());
                fileSelected = 0;
                refreshFilesListing();
                return;
            }
            if (!filesDirs.empty() && a == TAB_NEXT) {
                filesDirIndex = (filesDirIndex + 1) % int(filesDirs.size());
                fileSelected = 0;
                refreshFilesListing();
                return;
            }
            if (a == REFRESH) {
                reconcileStatusRequest = true;
                filesRefreshRequest = true;
                return;
            }
            if (!filesEntries.empty()) {
                int nFiles = int(filesEntries.size());
                if (a == UP) fileSelected = std::max(0, fileSelected - 1);
                else if (a == DOWN) fileSelected = std::min(nFiles - 1, fileSelected + 1);
                else if (a == PAGE_PREV) fileSelected = std::max(0, fileSelected - 6);
                else if (a == PAGE_NEXT) fileSelected = std::min(nFiles - 1, fileSelected + 6);
                else if (a == CONFIRM) {
                    const auto& fe = filesEntries[fileSelected];
                    openDeleteConfirmation(fe.path, fe.size, findTaskIdForPath(fe.path));
                }
            }
            return;
        }

        if (keyboard) {
            const char* keys = "abcdefghijklmnopqrstuvwxyz0123456789.:/-_";
            const int totalKeys = 45; // 41 chars + Space(41), Back(42), Clear(43), OK(44)
            if (a == BACK) { keyboard = false; kbTarget = KbNone; return; }
            if (a == LEFT) key = (key + totalKeys - 1) % totalKeys;
            else if (a == RIGHT) key = (key + 1) % totalKeys;
            else if (a == UP) key = (key + totalKeys - 9) % totalKeys;
            else if (a == DOWN) key = (key + 9) % totalKeys;
            else if (a == CONFIRM) {
                if (key < 41 && kbBuffer.size() < 120) kbBuffer += keys[key];
                else if (key == 41 && kbBuffer.size() < 120) kbBuffer += ' ';
                else if (key == 42 && !kbBuffer.empty()) kbBuffer.pop_back();
                else if (key == 43) kbBuffer.clear();
                else if (key == 44) {
                    if (kbTarget == KbSearch) {
                        query = kbBuffer;
                        selected = 0;
                        filter();
                    } else if (kbTarget == KbServer) {
                        newServerUrl = kbBuffer;
                        serverSaveRequest = true;
                    }
                    keyboard = false;
                    kbTarget = KbNone;
                }
            }
            return;
        }

        if (serverPicker) {
            if (a == BACK) { serverPicker = false; return; }
            if (a == UP) serverChoice = (serverChoice + 3) % 4;
            else if (a == DOWN) serverChoice = (serverChoice + 1) % 4;
            else if (a == CONFIRM) {
                serverPicker = false;
                if (serverChoice == 0) {
                    kbTarget = KbServer;
                    kbBuffer = serverUrl;
                    serverEditRequest = true;
                } else if (serverChoice == 1) {
                    serverSyncRequest = true;
                } else if (serverChoice == 2) {
                    serverResetRequest = true;
                }
            }
            return;
        }

        if (a == DOWNLOADS) {
            tab = (tab == dlTab) ? 0 : dlTab;
            detail = false;
            if (tab == dlTab) reconcileStatusRequest = true;
            filter();
            return;
        }

        if (tab == dlTab) {
            if (a == BACK) { tab = 0; filter(); return; }
            if (a == TAB_PREV) { tab = (tab + totalTabs() - 1) % totalTabs(); selected = 0; filter(); return; }
            if (a == TAB_NEXT) { tab = 0; selected = 0; filter(); return; }
            if (a == SORT) { openFilesManager("/data/pkg"); return; }
            if (a == LOCAL_INSTALL) {
                if (!tasks.empty()) {
                    const auto& t = tasks[std::min(taskSelected, int(tasks.size()) - 1)];
                    if (t.mode == InstallMode::DirectSave && !t.path.empty()) {
                        uint64_t sz = 0;
                        if (portalRegularFileStatus(t.path, sz) == 1) {
                            openDeleteConfirmation(t.path, sz > 0 ? sz : t.bytes, t.id);
                            return;
                        }
                        reconcileStatusRequest = true;
                        size_t sl = t.path.rfind('/');
                        std::string dir = (sl != std::string::npos && sl > 0) ? t.path.substr(0, sl) : "/data/pkg";
                        openFilesManager(dir);
                        return;
                    }
                }
                openFilesManager("/data/pkg");
                return;
            }
            if (!tasks.empty()) {
                if (a == UP) taskSelected = std::max(0, taskSelected - 1);
                else if (a == DOWN) taskSelected = std::min(int(tasks.size()) - 1, taskSelected + 1);
                else if (a == CONFIRM) { actionTaskId = selectedTaskId(); taskToggleRequest = true; }
                else if (a == SEARCH) { actionTaskId = selectedTaskId(); taskRetryRequest = true; }
                else if (a == FAVORITE) { actionTaskId = selectedTaskId(); taskCancelRequest = true; }
                else if (a == REMOVE_TASK) { actionTaskId = selectedTaskId(); taskRemoveRequest = true; }
            }
            return;
        }

        if (detail) {
            const Game* cur = current();
            int nLinks = (cur && !cur->links.empty()) ? int(cur->links.size()) : 1;
            int totalChoices = (nLinks > 1) ? (nLinks + 1) : 1;
            if (detailLink < 0 || detailLink >= totalChoices) detailLink = 0;

            auto prepareDetailBatch = [&](const Game& item) {
                installBatch.clear();
                if (nLinks > 1 && detailLink == nLinks) {
                    for (int li = 0; li < nLinks; ++li) {
                        installBatch.push_back(gameForLink(item, li));
                    }
                    installItem = installBatch[0];
                } else {
                    installItem = gameForLink(item, detailLink);
                    installBatch.push_back(installItem);
                }
            };

            if (a == BACK) { detail = false; return; }
            if (a == FAVORITE) { favorite(); return; }
            if (a == SORT) {
                refreshDetailFileProbe();
                openFilesManager(detailDirectDir.empty() ? std::string("/data/pkg") : detailDirectDir);
                return;
            }
            if (nLinks > 1 && (a == UP || a == TAB_PREV)) {
                detailLink = (detailLink + totalChoices - 1) % totalChoices;
                refreshDetailFileProbe();
                return;
            }
            if (nLinks > 1 && (a == DOWN || a == TAB_NEXT)) {
                detailLink = (detailLink + 1) % totalChoices;
                refreshDetailFileProbe();
                return;
            }
            if (nLinks > 1 && a == PAGE_PREV) {
                detailLink = std::max(0, detailLink - 4);
                refreshDetailFileProbe();
                return;
            }
            if (nLinks > 1 && a == PAGE_NEXT) {
                detailLink = std::min(totalChoices - 1, detailLink + 4);
                refreshDetailFileProbe();
                return;
            }
            if (a == LOCAL_INSTALL && cur && !(nLinks > 1 && detailLink == nLinks) &&
                gameForLink(*cur, detailLink).directDownload) {
                refreshDetailFileProbe();
                if (detailDirectExists && !detailDirectPath.empty()) {
                    openDeleteConfirmation(detailDirectPath, detailDirectSize, findTaskIdForPath(detailDirectPath));
                } else {
                    openFilesManager(detailDirectDir.empty() ? std::string("/data/pkg") : detailDirectDir);
                }
                return;
            }
            if ((a == CONFIRM || a == LOCAL_INSTALL || a == BINLOADER_INSTALL) && cur) {
                prepareDetailBatch(*cur);
                if (a == CONFIRM) installBgRequest = true;
                else if (a == LOCAL_INSTALL) installLocalRequest = true;
                else installBinLoaderRequest = true;
                return;
            }
            return;
        }

        if (a == SERVER_CONFIG) {
            serverPicker = true;
            serverChoice = 0;
            return;
        }
        if (a == SEARCH) {
            keyboard = true;
            kbTarget = KbSearch;
            kbBuffer = query;
            key = 0;
            return;
        }
        if (a == FAVORITE) { favorite(); return; }
        if (a == BACK) {
            if (!query.empty()) { query.clear(); filter(); }
            else if (tab != 0) { tab = 0; filter(); }
            return;
        }
        if (a == TAB_PREV || a == TAB_NEXT) {
            int n = totalTabs();
            tab = (tab + (a == TAB_NEXT ? 1 : n - 1)) % n;
            selected = 0;
            filter();
            return;
        }
        if (a == SORT) { sort = (sort + 1) % 4; filter(); return; }

        int n = int(visible.size());
        if (!n) return;
        if (a == LEFT) selected = std::max(0, selected - 1);
        else if (a == RIGHT) selected = std::min(n - 1, selected + 1);
        else if (a == UP) selected = std::max(0, selected - 6);
        else if (a == DOWN) selected = std::min(n - 1, selected + 6);
        else if (a == PAGE_PREV) selected = std::max(0, selected - 12);
        else if (a == PAGE_NEXT) selected = std::min(n - 1, selected + 12);
        else if (a == CONFIRM) { detail = true; detailLink = 0; refreshDetailFileProbe(); }
    }

    void drawCase(const Game& g, int x, int y, int side) {
        const auto& b = cover(g);
        const int frameW = caseFrame.p.empty() ? side * 77 / 100 : side * caseFrame.w / caseFrame.h;
        const int frameX = x + (side - frameW) / 2;
        const int artX = frameX + frameW * 25 / 1024;
        const int artY = y + side * 183 / 1325;
        const int artW = frameW * (1000 - 25) / 1024;
        const int artH = side * (1290 - 183) / 1325;

        c.rounded(frameX - 2, y + 3, frameW + 4, side, 7, 0x02050a);
        c.rect(artX, artY, artW, artH, 0x070c14);

        if (!b.p.empty()) {
            int trim = 0;
            auto blueRow = [&](int row) {
                int blue = 0, total = 0;
                for (int i = 0; i < b.w; i += 3) {
                    auto p = &b.p[(row * b.w + i) * 3];
                    if (p[2] > p[0] * 1.35f && p[2] > p[1] * 1.05f) blue++;
                    total++;
                }
                return total ? blue * 100 / total : 0;
            };
            if (blueRow(std::min(b.h - 1, 3)) > 30) {
                int max = b.h * 18 / 100;
                trim = b.h * 7 / 100;
                for (int row = trim; row < max; row++) {
                    if (blueRow(row) < 15) { trim = row; break; }
                }
            }
            float scale = std::max(float(artW) / b.w, float(artH) / std::max(1, b.h - trim));
            int sw = std::min(b.w, int(artW / scale));
            int sh = std::min(std::max(1, b.h - trim), int(artH / scale));
            int sx = (b.w - sw) / 2;
            int sy = trim + (std::max(1, b.h - trim) - sh) / 3;
            c.region(b, sx, sy, sw, sh, artX, artY, artW, artH);
        } else {
            c.rect(artX, artY, artW, artH, 0x0c1526);
            c.text("HOMEBREW", artX + 14, artY + 34, 15, 0x318ee0, artW - 24);
            c.lines(g.title, artX + 14, artY + 66, 19, 0xe7eff7, artW - 28, 3);
        }

        if (!caseFrame.p.empty()) {
            c.image(caseFrame, frameX, y, frameW, side);
        } else {
            c.rounded(frameX, y, frameW, side, 8, 0x0b2850);
            c.rounded(frameX + 2, y + 2, frameW - 4, side - 4, 6, 0x28569c);
            c.rect(frameX + 4, y + 4, frameW - 8, side * 12 / 100, 0x0c3c78);
            c.text("HB", frameX + 13, y + 26, 18, 0xf5f9ff, 60);
        }
    }

    void draw() {
        const uint32_t bg = 0x060b12, panel = 0x0b121e, muted = 0xb2bec9, white = 0xe7eff7, cyan = 0x318ee0, rule = 0x202d42;
        const int dlTab = downloadsTab();
        c.rect(0, 0, 1920, 1080, bg);

        // Cabeçalho superior
        c.rect(315, 0, 1605, 204, 0x080e18);
        c.rect(315, 203, 1605, 1, 0x1a2638);
        c.rect(315, 0, 2, 204, 0x0f1c30);
        c.rect(314, 0, 1, 989, rule);

        // Barra lateral (marca + categorias dinâmicas)
        c.image(logo, 24, 18, 64, 64);
        c.text(storeName, 98, 62, 30, white, 208);
        c.text(storeSubtitle, 32, 108, 15, cyan, 265);
        c.text("CATEGORIAS", 32, 155, 18, muted);

        int maxVisibleTabs = 10;
        int nTabs = totalTabs();
        int scrollStart = 0;
        if (nTabs > maxVisibleTabs) {
            scrollStart = std::max(0, std::min(tab - maxVisibleTabs / 2, nTabs - maxVisibleTabs));
        }
        for (int row = 0; row < std::min(maxVisibleTabs, nTabs); ++row) {
            int i = scrollStart + row;
            int y = 205 + row * 52;
            bool active = (i == tab);
            if (active) {
                c.rounded(22, y - 28, 270, 42, 6, 0x101c2d);
                c.rect(22, y - 28, 3, 42, cyan);
            }
            std::string label = tabTitle(i);
            c.text(label, 42, y, active ? 23 : 22, active ? white : muted, i == dlTab ? 170 : 196);

            std::string countStr;
            if (i == 0) countStr = std::to_string(games.size());
            else if (i == 1) countStr = std::to_string(favorites.size());
            else if (i >= 2 && i < dlTab) countStr = std::to_string(countForCategory(categories[i - 2]));
            else if (i == dlTab) {
                int actDl = countActiveDirectTasks();
                if (actDl > 0) {
                    c.spinner(224, y - 7, 9, 2, spinnerAngle(), 0x163154, 0x5cb8ff);
                    if (actDl == 1) {
                        if (const auto* act = primaryActiveDirectTask()) countStr = formatPercentText(taskDisplayPercent(*act));
                    } else {
                        countStr = std::to_string(actDl) + " at.";
                    }
                } else {
                    countStr = std::to_string(tasks.size());
                }
            }
            c.text(countStr, 240, y - 1, 17, active ? cyan : muted, 52);
        }

        // Info do Servidor na barra lateral
        c.rect(24, 750, 266, 1, rule);
        c.text("L1 / R1  Categoria", 32, 790, 18, muted, 260);
        c.text("L3  Servidor:", 32, 830, 18, cyan, 260);
        c.text(urlHostLabel(serverUrl), 32, 860, 21, white, 265);
        c.text("OPTIONS  Sincronizar", 32, 902, 18, muted, 260);
        c.text(std::to_string(games.size()) + " itens  •  " + std::to_string(categories.size()) + " cat.", 32, 955, 19, muted, 265);

        // Cabeçalho principal
        c.text(storeName + " /", 352, 62, 20, cyan, 260);
        c.text(tabTitle(tab), 530, 62, 20, white, 500);
        c.text(query.empty() ? "Loja WILLGAME PS4" : "Busca: " + query, 352, 124, 44, white, 1220);

        const char* sorts[] = {"Mais recentes", "Nome A-Z", "Menor tamanho", "Maior tamanho"};
        c.rounded(1622, 25, 276, 42, 5, 0x0d1626);
        c.rect(1622, 25, 276, 1, rule);
        c.rect(1622, 66, 276, 1, rule);
        c.text("ORDEM  " + std::string(sorts[sort]), 1636, 52, 18, muted, 250);
        c.text(std::to_string(visible.size()) + " itens   |   " + status, 352, 173, 20, muted, 1530);

        // Grid do catálogo
        if (tab != dlTab) {
            drawHeaderActivityPill(dlTab);
            if (!(detail && current())) {
                if (visible.empty()) {
                    c.text(games.empty() ? "Nenhum homebrew sincronizado" : "Nenhum item encontrado", 352, 390, 40, white);
                    std::string hint = games.empty()
                        ? "Inicie o Painel Web e pressione OPTIONS para sincronizar (" + serverUrl + ") ou L3 para trocar o IP."
                        : (tab == 1 ? "Pressione QUADRADO em um homebrew para salvar nos favoritos."
                                    : "Pressione TRIÂNGULO para buscar ou CÍRCULO para limpar filtros.");
                    c.lines(hint, 352, 448, 24, muted, 1400, 3);
                } else {
                    int start = selected / 12 * 12;
                    for (int i = start; i < std::min(start + 12, int(visible.size())); ++i) {
                        const auto& g = games[visible[i]];
                        int k = i - start;
                        int x = 352 + (k % 6) * 257;
                        int y = 214 + (k / 6) * 362;
                        bool active = (i == selected);
                        if (active) c.rounded(x - 2, y - 2, 238, 332, 6, cyan);
                        c.rounded(x, y, 234, 328, 5, panel);
                        drawCase(g, x, y, 234);
                        if (const InstallPackage* cardTask = findDirectTaskForItem(g.id)) {
                            int cardPct = taskDisplayPercent(*cardTask);
                            if (isTaskActive(*cardTask)) {
                                c.rounded(x + 12, y + 194, 210, 32, 6, 0x061326);
                                c.rect(x + 12, y + 194, 3, 32, cyan);
                                c.spinner(x + 30, y + 210, 9, 3, spinnerAngle(), 0x163154, 0x5cb8ff);
                                std::string pStr = formatPercentText(cardPct);
                                int pW = c.textWidth(pStr, 17);
                                c.text(pStr, x + 214 - pW, y + 216, 17, 0x5cb8ff, pW + 6);
                                c.text("Baixando", x + 46, y + 215, 16, white, std::max(60, 162 - pW));
                            } else if (cardTask->state == InstallState::Complete) {
                                c.rounded(x + 12, y + 196, 210, 28, 6, 0x08221e);
                                c.rect(x + 12, y + 196, 3, 28, 0x34d399);
                                c.text("100%  •  Salvo", x + 24, y + 216, 15, 0x34d399, 190);
                            }
                        }

                        std::string ver = g.version.empty() ? "HB" : "v" + g.version;
                        c.text(ver, x + 11, y + 257, 17, white, 75);
                        c.text(g.category, x + 86, y + 257, 16, cyan, 126);
                        if (favorites.count(g.id)) c.text("*", x + 216, y + 257, 18, cyan);
                        c.lines(g.title, x + 11, y + 282, 19, white, 212, 2);
                        std::string sub = g.size.empty() ? g.author : g.size;
                        c.text(sub, x + 11, y + 322, 17, muted, 212);
                    }
                    c.text("Página " + std::to_string(selected / 12 + 1) + " / " + std::to_string((visible.size() + 11) / 12),
                           352, 965, 22, muted);
                }
            }
        } else {
            // Aba Notificações / Fila de Instalação
            c.rect(315, 0, 1605, 989, bg);
            c.rect(314, 0, 1, 989, rule);
            c.text(storeName + " /", 352, 62, 20, cyan, 260);
            c.text("Notificações & Fila", 530, 62, 20, white, 400);
            c.text("Notificações & Instalações", 352, 148, 44, white);
            c.text("INSTALAÇÃO EM BACKGROUND NO PS4  •  SISTEMA BGFT", 352, 190, 18, cyan, 1200);
            drawHeaderActivityPill(dlTab);

            if (tasks.empty()) {
                c.text("Nenhuma instalação na fila", 352, 395, 40, white);
                c.text("Abra qualquer homebrew e pressione X para iniciar em Notificações do PS4.", 352, 452, 26, muted);
            } else {
                taskSelected = std::max(0, std::min(taskSelected, int(tasks.size()) - 1));
                int start = taskSelected / 5 * 5;
                for (int i = start; i < std::min(start + 5, int(tasks.size())); ++i) {
                    const auto& t = tasks[i];
                    int y = 225 + (i - start) * 142;
                    bool active = (i == taskSelected);
                    bool isDirect = (t.mode == InstallMode::DirectSave);
                    c.rect(352, y, 1500, 128, active ? 0x12325c : panel);
                    if (active) c.rect(352, y, 5, 128, cyan);

                    std::string modeBadge = t.mode == InstallMode::RemoteBgft   ? "Notificações PS4"
                                          : t.mode == InstallMode::BinLoaderElf ? "Payload ELF"
                                          : isDirect                            ? "Download Direto"
                                                                                : "Download Local";
                    c.text(modeBadge + (t.taskId >= 0 ? "  #" + std::to_string(t.taskId) : ""), 1460, y + 28, 18, muted, 240);
                    c.text(installStateLabel(t), 1460, y + 58, 22,
                           t.state == InstallState::Error ? 0xffa5a5 : (t.state == InstallState::Complete ? 0x34d399 : cyan), 230);

                    if (isDirect) {
                        bool tAct = isTaskActive(t);
                        int tPct = taskDisplayPercent(t);
                        drawActivityIcon(t, 408, y + 64, 24, 5, false);
                        c.text(t.name, 456, y + 34, 26, white, 970);

                        std::string pctStr = formatPercentText(tPct);
                        int pctW = c.textWidth(pctStr, 34);
                        uint32_t pctCol = t.state == InstallState::Complete ? 0x34d399
                                        : t.state == InstallState::Error    ? 0xffa5a5
                                        : t.state == InstallState::Paused   ? 0xfbbf24 : 0x5cb8ff;
                        c.text(pctStr, 1830 - pctW, y + 72, 34, pctCol, pctW + 8);

                        drawProgressBar(456, y + 52, 970, 8, tPct, tAct, 0x050912,
                                        t.state == InstallState::Complete ? 0x34d399 : cyan);

                        std::string sizePart = t.bytes > 0 ? (bytesText(t.doneBytes) + " / " + bytesText(t.bytes))
                                             : t.doneBytes > 0 ? (bytesText(t.doneBytes) + " baixados")
                                                               : "Calculando tamanho...";
                        std::string stats = "DIRETO   •   " + sizePart + (tPct >= 0 ? "   (" + pctStr + ")" : "");
                        c.text(stats, 456, y + 88, 20, white, 970);
                        c.text(installStatusText(t), 456, y + 116, 19,
                               t.state == InstallState::Error ? 0xffa5a5 : muted, 1200);
                    } else {
                        c.text(t.name, 377, y + 36, 26, white, 1050);
                        std::string infoId = t.contentId.empty() ? "PKG" : t.contentId;
                        if (t.bytes > 0) infoId += "   •   " + bytesText(t.bytes);
                        c.text(infoId, 377, y + 76, 20, muted, 1050);
                        c.text(installStatusText(t), 377, y + 110, 20,
                               t.state == InstallState::Error ? 0xffa5a5 : white, 1280);
                    }
                }
            }
        }

        // Rodapé com legenda do controle
        c.rect(0, 989, 1920, 91, bg);
        c.rect(0, 989, 1920, 1, 0x334157);
        if (tab != dlTab) {
            c.text("X  Detalhes", 352, 1032, 21, white);
            c.text("TRIÂNGULO  Buscar", 510, 1032, 21, white);
            c.text("QUADRADO  Favorito", 745, 1032, 21, white);
            c.text("L2/R2  Página", 995, 1032, 21, white);
            c.text("TOUCHPAD  Ordem", 1185, 1032, 21, white);
            c.text("R3  Notificações", 1410, 1032, 21, white);
            c.text("OPTIONS  Sincronizar", 1630, 1032, 21, white);
        } else {
            c.text("X  Pausar/Retomar", 352, 1032, 20, white);
            c.text("TRIÂNGULO  Tentar", 575, 1032, 20, white);
            c.text("QUADRADO  Cancelar", 785, 1032, 20, white);
            c.text("OPTIONS  Remover", 1015, 1032, 20, white);
            c.text("L3  Excluir arquivo", 1225, 1032, 20, cyan);
            c.text("TOUCHPAD  Arquivos", 1455, 1032, 20, cyan);
            c.text("CÍRCULO  Voltar", 1685, 1032, 20, white);
        }

        // Modal de Detalhes
        if (detail && current()) {
            const auto& g = *current();
            int nLinks = !g.links.empty() ? int(g.links.size()) : 1;
            int totalChoices = (nLinks > 1) ? (nLinks + 1) : 1;
            int selLink = std::max(0, std::min(detailLink, totalChoices - 1));
            bool isAll = (nLinks > 1 && selLink == nLinks);
            Game shown = isAll ? g : gameForLink(g, selLink);

            c.rect(340, 190, 1560, 790, 0x06254a);
            c.rect(340, 190, 7, 790, cyan);
            const auto& b = cover(g);
            if (!b.p.empty()) {
                c.image(b, 382, 235, 390, 510);
            } else {
                c.rect(382, 235, 390, 510, panel);
                c.text("HOMEBREW PKG", 412, 290, 22, cyan);
                c.lines(g.title, 412, 345, 28, white, 330, 4);
            }
            drawDetailProgressWidget(g, shown, isAll);

            std::string metaTop = g.category + (g.version.empty() ? "" : "  /  v" + g.version) +
                                  (g.author.empty() ? "" : "  /  " + g.author);
            c.text(metaTop, 810, 255, 22, cyan, 1042);
            c.lines(g.title, 810, 320, 44, white, 1042, 2);

            std::string cidShow = isAll
                ? ("Todos os " + std::to_string(nLinks) + " links na fila")
                : (shown.contentId.empty() ? "Content ID detectado ao instalar" : shown.contentId);
            std::string sizeShow = isAll
                ? (g.size.empty() ? std::to_string(nLinks) + " arquivos" : g.size)
                : (shown.size.empty() ? "Tamanho auto" : shown.size);
            std::string subInfo = sizeShow + "   •   " + cidShow;
            if (g.splitUrls.size() > 1) {
                subInfo += "   •   " + std::to_string(g.splitUrls.size()) + " partes";
            }
            c.text(subInfo, 810, 435, 23, muted, 1042);

            if (!g.description.empty()) {
                c.lines(g.description, 810, 480, 22, white, 1042, nLinks > 1 ? 2 : 4);
            }

            if (nLinks > 1) {
                int maxBtns = 4;
                bool hasScroll = totalChoices > maxBtns;
                int rowStart = hasScroll ? std::max(0, std::min(selLink - 1, totalChoices - maxBtns)) : 0;
                std::string hintHdr = hasScroll
                    ? ("LINKS (" + std::to_string(selLink + 1) + "/" + std::to_string(totalChoices) + ")  —  Cima/Baixo ou L2/R2 para rolar, X para instalar:")
                    : ("SELECIONE O LINK (" + std::to_string(selLink + 1) + "/" + std::to_string(totalChoices) + ")  —  Cima/Baixo e X para instalar:");
                if (!isAll && shown.directDownload) {
                    std::string dstShow = shown.directPath.empty() ? std::string("/data/pkg") : shown.directPath;
                    hintHdr = "LINKS (" + std::to_string(selLink + 1) + "/" + std::to_string(totalChoices) + ")  —  X baixa direto para " + dstShow + ":";
                } else if (!isAll && shown.useBinloader) {
                    hintHdr = "LINKS (" + std::to_string(selLink + 1) + "/" + std::to_string(totalChoices) + ")  —  X envia Payload ELF (127.0.0.1:9090):";
                }
                c.text(hintHdr, 810, 540, 18, cyan, 1042);
                int btnW = hasScroll ? 1018 : 1042;
                for (int r = 0; r < std::min(maxBtns, totalChoices); ++r) {
                    int idx = rowStart + r;
                    int by = 556 + r * 60;
                    bool activeBtn = (idx == selLink);
                    const InstallPackage* lt = (idx < nLinks) ? findDirectTaskForItem(g.id, g.links[idx].url) : nullptr;
                    int batchActive = (idx == nLinks) ? countActiveDirectTasks(g.id) : 0;

                    c.rect(810, by, btnW, 50, activeBtn ? cyan : 0x12325c);
                    if (!activeBtn) c.rect(810, by, 5, 50, cyan);
                    if (lt && isTaskActive(*lt)) {
                        drawProgressBar(810, by + 46, btnW, 4, taskDisplayPercent(*lt), true,
                                        activeBtn ? 0x16538f : 0x0a1c34, activeBtn ? bg : 0x5cb8ff);
                    }
                    std::string bName = idx == nLinks
                        ? "Instalar TODOS (" + std::to_string(nLinks) + " links na fila)"
                        : (g.links[idx].label.empty() ? "Link " + std::to_string(idx + 1) : g.links[idx].label);
                    if (idx < nLinks && g.links[idx].directDownload) bName += "  [DIRETO]";
                    else if (idx < nLinks && g.links[idx].useBinloader) bName += "  [ELF]";
                    std::string bText = (activeBtn ? "X  " : "   ") + bName;
                    bool hasRowStatus = (lt != nullptr) || (idx == nLinks && batchActive > 0);
                    int textMaxW = hasRowStatus ? (hasScroll ? 645 : 668) : (hasScroll ? 812 : 836);
                    c.text(bText, 830, by + 33, 24, activeBtn ? bg : white, textMaxW);

                    if (lt) {
                        drawActivityIcon(*lt, 1514, by + 25, 11, 3, activeBtn);
                        std::string rowStat = lt->state == InstallState::Error
                                            ? "Erro"
                                            : formatPercentText(taskDisplayPercent(*lt));
                        uint32_t rowCol = activeBtn ? bg
                                        : (lt->state == InstallState::Complete ? 0x34d399
                                        : lt->state == InstallState::Error     ? 0xffa5a5 : 0x5cb8ff);
                        c.text(rowStat, 1534, by + 32, 19, rowCol, 108);
                    } else if (idx == nLinks && batchActive > 0) {
                        c.spinner(1514, by + 25, 11, 3, spinnerAngle(),
                                  activeBtn ? 0x16538f : 0x163154, activeBtn ? bg : 0x5cb8ff);
                        c.text(std::to_string(batchActive) + " at.", 1534, by + 32, 19,
                               activeBtn ? bg : 0x5cb8ff, 108);
                    }
                    if (idx < nLinks && !g.links[idx].size.empty()) {
                        c.text(g.links[idx].size, hasScroll ? 1650 : 1674, by + 32, 19, activeBtn ? bg : muted, 155);
                    }
                }
                if (hasScroll) {
                    const int sbX = 1842, sbY = 556, sbW = 10, sbH = 230;
                    c.rect(sbX, sbY, sbW, sbH, 0x081322);
                    int thumbH = std::max(28, sbH * maxBtns / totalChoices);
                    int maxOffset = std::max(1, totalChoices - maxBtns);
                    int thumbY = sbY + (sbH - thumbH) * rowStart / maxOffset;
                    c.rect(sbX, thumbY, sbW, thumbH, cyan);
                    if (rowStart > 0) {
                        for (int k = 0; k < 5; ++k) c.rect(sbX + 4 - k, sbY - 12 + k, k * 2 + 2, 1, cyan);
                    }
                    if (rowStart + maxBtns < totalChoices) {
                        for (int k = 0; k < 5; ++k) c.rect(sbX + 4 - k, sbY + sbH + 10 - k, k * 2 + 2, 1, cyan);
                    }
                }
                c.text(favorites.count(g.id) ? "Salvo nos favoritos (QUADRADO remove)" : "QUADRADO  Salvar nos favoritos",
                       810, 816, 20, cyan, 1020);
                if (!isAll && shown.directDownload) {
                    std::string dirShow = detailDirectDir.empty() ? std::string("/data/pkg") : detailDirectDir;
                    std::string l3Act = detailDirectExists
                        ? ("L3  Excluir arquivo (" + bytesText(detailDirectSize) + ")")
                        : ("L3 / TOUCHPAD  Arquivos em " + dirShow);
                    c.text(l3Act + "     OPTIONS  Atualizar     CÍRCULO  Voltar", 810, 850, 20, cyan, 1042);
                } else {
                    c.text("L3  Baixar no App + Instalar (Local)     R3  Notificações     OPTIONS  Atualizar     CÍRCULO  Voltar",
                           810, 850, 20, white, 1042);
                }
                c.lines(isAll ? g.url : shown.url, 810, 892, 19, muted, 1042, 2);
            } else {
                c.text(favorites.count(g.id) ? "Salvo nos favoritos (QUADRADO remove)" : "QUADRADO  Salvar nos favoritos",
                       810, 645, 22, cyan, 1020);
                const InstallPackage* singleTask = findDirectTaskForItem(g.id, shown.url);
                if (!singleTask && shown.directDownload) singleTask = findDirectTaskForItem(g.id, "");
                bool singleActive = singleTask && isTaskActive(*singleTask);
                if (singleActive) {
                    int sPct = taskDisplayPercent(*singleTask);
                    drawProgressBar(810, 675, 1042, 66, sPct, true, 0x0d3666, 0x165191);
                    drawProgressBar(810, 735, 1042, 6, sPct, true, 0x071a33, 0x5cb8ff);
                    drawActivityIcon(*singleTask, 846, 706, 16, 4, false);
                    std::string sPctStr = formatPercentText(sPct);
                    int sPctW = c.textWidth(sPctStr, 30);
                    c.text(sPctStr, 1826 - sPctW, 718, 30, 0x5cb8ff, sPctW + 8);
                    std::string actLabel = "Baixando direto para " +
                        (singleTask->path.empty() ? std::string("/data/pkg") : singleTask->path) + "...";
                    c.text(actLabel, 876, 716, 25, white, std::max(240, 930 - sPctW));
                } else {
                    c.rect(810, 675, 1042, 66, cyan);
                    std::string mainActionLabel = shown.directDownload
                        ? ("X  Baixar Arquivo Direto (" + (shown.directPath.empty() ? std::string("/data/pkg") : shown.directPath) + ")")
                        : shown.useBinloader
                              ? "X  Enviar via Payload ELF (BinLoader 127.0.0.1:9090)"
                              : "X  Instalar em Background (Notificações PS4)";
                    c.text(mainActionLabel, 836, 720, 28, bg, 990);
                }
                if (shown.directDownload) {
                    std::string dirShow = detailDirectDir.empty() ? std::string("/data/pkg") : detailDirectDir;
                    std::string line1 = detailDirectExists
                        ? ("L3  Excluir arquivo salvo (" + bytesText(detailDirectSize) + ")     R3  Notificações")
                        : ("L3  Gerenciar/Excluir em " + dirShow + "     R3  Notificações");
                    c.text(line1, 810, 780, 22, detailDirectExists ? uint32_t(0xffa5a5) : cyan, 1042);
                    c.text(singleActive
                               ? "OPTIONS  Atualizar     CÍRCULO  Voltar (continua baixando)"
                               : "TOUCHPAD  Pasta     OPTIONS  Atualizar status     CÍRCULO  Voltar",
                           810, 820, 20, muted, 1042);
                } else {
                    c.text(singleActive
                               ? "L3  Baixar no App (Local)     R3  Notificações     CÍRCULO  Voltar"
                               : "L3  Baixar no App + Instalar (Local)     R3  Ver Notificações",
                           810, 780, 24, white, 1042);
                    if (!singleActive) {
                        c.text("OPTIONS  Atualizar status     CÍRCULO  Voltar ao catálogo", 810, 822, 22, muted);
                    }
                }
                c.lines(shown.url, 810, 868, 20, muted, 1042, 2);
            }
        }

        // Modal Configurar Servidor
        if (serverPicker) {
            c.rect(365, 230, 1230, 640, 0x091c34);
            c.rect(365, 230, 7, 640, cyan);
            c.text("Painel Web de Sincronização", 420, 295, 40, white);
            c.text("Servidor atual: " + serverUrl, 420, 348, 24, cyan, 1120);
            c.text("Sincroniza categorias, links de .pkg e capas diretamente do seu painel.", 420, 390, 22, muted, 1120);
            const char* opts[] = {
                "Editar IP / URL do servidor (Teclado PS4)",
                "Sincronizar catálogo agora",
                "Restaurar padrão (http://192.168.1.2:8765)",
                "Voltar"
            };
            for (int i = 0; i < 4; ++i) {
                int y = 440 + i * 82;
                c.rect(420, y, 1120, 64, i == serverChoice ? cyan : 0x12325c);
                c.text(opts[i], 448, y + 43, 26, i == serverChoice ? bg : white, 1060);
            }
            c.text("X  Selecionar     CÍRCULO  Fechar", 420, 825, 24, muted);
        }

        // Teclado Virtual Fallback (Busca ou URL)
        if (keyboard) {
            c.rect(350, 205, 1260, 745, 0x091c34);
            c.rect(350, 205, 7, 745, cyan);
            c.text(kbTarget == KbServer ? "Endereço do Painel Web" : "Buscar Homebrew", 400, 265, 38, white);
            c.rect(400, 295, 1150, 62, 0x050912);
            c.text(kbBuffer + "_", 422, 338, 30, white, 1100);

            const char* keys = "abcdefghijklmnopqrstuvwxyz0123456789.:/-_";
            for (int i = 0; i < 45; ++i) {
                int x = 400 + (i % 9) * 128;
                int y = 390 + (i / 9) * 86;
                c.rect(x, y, 114, 70, i == key ? cyan : 0x12325c);
                std::string label;
                if (i < 41) label = std::string(1, keys[i]);
                else if (i == 41) label = "Espaço";
                else if (i == 42) label = "Apagar";
                else if (i == 43) label = "Limpar";
                else label = "OK";
                c.text(label, x + 12, y + 45, i < 41 ? 30 : 21, i == key ? bg : white, 100);
            }
            c.text("D-pad  Mover     X  Digitar     CÍRCULO  Fechar", 400, 890, 24, muted);
        }

        // Modal Gerenciar / Excluir Arquivos Baixados nos Caminhos Pré-Selecionados
        if (filesModal) {
            c.rect(290, 140, 1340, 760, 0x091c34);
            c.rect(290, 140, 7, 760, cyan);
            c.text("Arquivos Baixados (Caminhos Pré-Selecionados)", 340, 202, 36, white, 1240);

            std::string curDir = filesDirs.empty() ? std::string("/data/pkg")
                                                   : filesDirs[std::max(0, std::min(filesDirIndex, int(filesDirs.size()) - 1))];
            std::string dirDisp = (curDir == "/data") ? "/data/" : curDir;
            c.rounded(340, 228, 1240, 48, 6, 0x0d2747);
            std::string navDir = "L1  <   Pasta (" + std::to_string(filesDirIndex + 1) + "/" +
                                 std::to_string(std::max(1, int(filesDirs.size()))) + "):  " + dirDisp + "   >  R1";
            c.text(navDir, 362, 260, 22, cyan, 940);
            std::string cntTxt = std::to_string(filesEntries.size()) + " arquivo(s)";
            int cntW = c.textWidth(cntTxt, 20);
            c.text(cntTxt, 1558 - cntW, 259, 20, white, cntW + 8);

            if (!filesError.empty()) {
                c.text(filesError, 340, 355, 24, 0xffa5a5, 1240);
            } else if (filesEntries.empty()) {
                c.text("Nenhum arquivo encontrado em " + dirDisp, 340, 390, 30, white, 1240);
                c.text("Use L1 / R1 para alternar entre /data/, /data/pkg e caminhos cadastrados no painel.", 340, 440, 21, muted, 1240);
            } else {
                const int maxRows = 6;
                int nFiles = int(filesEntries.size());
                fileSelected = std::max(0, std::min(fileSelected, nFiles - 1));
                int startIdx = (nFiles > maxRows) ? std::max(0, std::min(fileSelected - 2, nFiles - maxRows)) : 0;
                bool hasScroll = nFiles > maxRows;
                int rowW = hasScroll ? 1216 : 1240;
                for (int r = 0; r < std::min(maxRows, nFiles); ++r) {
                    int idx = startIdx + r;
                    const auto& fe = filesEntries[idx];
                    int ry = 298 + r * 82;
                    bool activeRow = (idx == fileSelected);
                    c.rect(340, ry, rowW, 68, activeRow ? cyan : 0x12325c);
                    if (!activeRow) c.rect(340, ry, 5, 68, cyan);

                    bool dlBusy = isPathDownloading(fe.path);
                    int tid = findTaskIdForPath(fe.path);
                    c.text((activeRow ? "X  " : "   ") + fe.name, 362, ry + 34, 24, activeRow ? bg : white, 820);
                    std::string meta = dlBusy ? "[Download em andamento]"
                                     : (tid > 0 ? "[Baixado pela loja]" : fe.path);
                    c.text(meta, 366, ry + 57, 16,
                           activeRow ? bg : (dlBusy ? uint32_t(0xfbbf24) : (tid > 0 ? uint32_t(0x34d399) : muted)), 820);

                    std::string szStr = bytesText(fe.size);
                    int szW = c.textWidth(szStr, 22);
                    c.text(szStr, 340 + rowW - 22 - szW, ry + 42, 22, activeRow ? bg : cyan, szW + 8);
                }
                if (hasScroll) {
                    const int sbX = 1568, sbY = 298, sbW = 10, sbH = 478;
                    c.rect(sbX, sbY, sbW, sbH, 0x081322);
                    int thumbH = std::max(36, sbH * maxRows / nFiles);
                    int maxOff = std::max(1, nFiles - maxRows);
                    int thumbY = sbY + (sbH - thumbH) * startIdx / maxOff;
                    c.rect(sbX, thumbY, sbW, thumbH, cyan);
                }
            }
            c.text("X  Excluir arquivo selecionado     L1/R1  Trocar pasta     OPTIONS  Atualizar     CÍRCULO  Fechar",
                   340, 862, 20, muted, 1240);
        }

        // Modal Confirmar Exclusão do Arquivo
        if (deleteConfirm) {
            c.rect(460, 315, 1000, 380, 0x0c1626);
            c.rect(460, 315, 7, 380, 0xff6b6b);
            c.text("Confirmar exclusão do arquivo?", 510, 382, 36, white, 900);
            c.text("O arquivo será apagado permanentemente do armazenamento do PS4:", 510, 424, 21, 0xffa5a5, 900);
            c.rounded(510, 446, 900, 112, 8, 0x060d18);
            c.lines(deleteTargetPath, 530, 488, 22, white, 860, 2);
            c.text("Tamanho: " + bytesText(deleteTargetSize), 530, 544, 19, cyan, 860);

            c.rounded(510, 594, 340, 56, 6, 0xc93b47);
            c.text("X  Confirmar exclusão", 542, 631, 24, white, 300);
            c.rounded(876, 594, 260, 56, 6, 0x12325c);
            c.text("CÍRCULO  Cancelar", 914, 631, 24, white, 220);
        }

        if (!notice.empty()) {
            c.rect(320, 922, 1550, 60, 0x143359);
            c.rect(320, 922, 5, 60, cyan);
            c.text(notice, 345, 962, 24, white, 1500);
        }
        dirty = false;
    }
};
