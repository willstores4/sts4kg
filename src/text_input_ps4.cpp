#include "text_input_ps4.h"
#include <orbis/Sysmodule.h>
#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#ifndef PORTAL_HOST
#include "network_ps4.h"
#define IME_LOG(...) portalNetworkTrace(__VA_ARGS__)
#else
#define IME_LOG(...) ((void)0)
#endif

static_assert(sizeof(OrbisImeDialogSetting) == 96 && offsetof(OrbisImeDialogSetting, inputTextBuffer) == 40,
              "Native IME parameter ABI");

static void imeEncode(const std::string& s, char16_t* out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < s.size() && used + 1 < capacity;) {
        unsigned c = (unsigned char)s[i++], cp = c;
        int extra = 0;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; extra = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; extra = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; extra = 3; }
        else if (c >= 0x80) cp = 0xfffd;

        bool valid = true;
        size_t next = i;
        for (int n = 0; n < extra; n++) {
            if (next >= s.size() || ((unsigned char)s[next] & 0xc0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | ((unsigned char)s[next++] & 63);
        }
        if (extra && valid) {
            i = next;
            if (cp < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u) ||
                cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
        } else if (extra) {
            cp = 0xfffd;
        }
        if (cp > 0xffff) {
            if (used + 2 >= capacity) break;
            cp -= 0x10000;
            out[used++] = char16_t(0xd800 + (cp >> 10));
            out[used++] = char16_t(0xdc00 + (cp & 1023));
        } else {
            out[used++] = char16_t(cp);
        }
    }
    out[used] = 0;
}

static std::string imeDecode(const char16_t* in, size_t capacity) {
    std::string out;
    for (size_t i = 0; i < capacity && in[i]; i++) {
        unsigned cp = in[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < capacity && in[i + 1] >= 0xdc00 && in[i + 1] <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (in[++i] - 0xdc00);
            } else {
                cp = 0xfffd;
            }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    }
    return out;
}

static bool imeError(const char* stage, int code, std::string& error) {
    error = std::string(stage) + ".";
    IME_LOG("native keyboard error: stage=%s code=0x%08X", stage, (unsigned)code);
    return false;
}

NativeTextInput::~NativeTextInput() {
    if (opened) {
        sceImeDialogAbort();
        sceImeDialogTerm();
    }
}

bool NativeTextInput::begin(int user, const std::string& initial, std::string& error,
                            const std::string& title, const std::string& placeholder, bool urlMode) {
    error.clear();
    if (opened) { error = "O teclado do PS4 já está aberto."; return false; }
    if (user < 0) { error = "Faça login em um usuário no PS4 para abrir o teclado."; return false; }
    if (!ready) {
        int r = (int)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
        IME_LOG("native keyboard CommonDialog module: 0x%08X", (unsigned)r);
        if (r) return imeError("Não foi possível carregar diálogos do PS4", r, error);
        r = sceSysmoduleLoadModule(ORBIS_SYSMODULE_LIBIME);
        if (r) return imeError("Não foi possível carregar entrada de texto", r, error);
        r = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        if (r) return imeError("Não foi possível carregar teclado do PS4", r, error);
        r = sceCommonDialogInitialize();
        IME_LOG("native keyboard CommonDialog initialize: 0x%08X", (unsigned)r);
        if (r) return imeError("Não foi possível inicializar diálogos", r, error);
        ready = true;
    }
    if (sceCommonDialogIsUsed()) {
        error = "Outro diálogo do PS4 está aberto.";
        return false;
    }
    memset(buffer, 0, sizeof buffer);
    memset(titleBuf, 0, sizeof titleBuf);
    memset(placeholderBuf, 0, sizeof placeholderBuf);
    imeEncode(initial, buffer, 129);
    imeEncode(title, titleBuf, 64);
    imeEncode(placeholder, placeholderBuf, 64);

    OrbisImeDialogSetting p = {};
    p.userId = (uint32_t)user;
    p.type = urlMode ? ORBIS_TYPE_TYPE_URL : ORBIS_TYPE_DEFAULT;
    p.enterLabel = urlMode ? ORBIS_BUTTON_LABEL_DEFAULT : ORBIS_BUTTON_LABEL_SEARCH;
    p.maxTextLength = 128;
    p.inputTextBuffer = reinterpret_cast<wchar_t*>(buffer);
    p.title = reinterpret_cast<const wchar_t*>(titleBuf);
    p.placeholder = reinterpret_cast<const wchar_t*>(placeholderBuf);

    int r = sceImeDialogInit(&p, nullptr);
    IME_LOG("native keyboard open: user=%d result=0x%08X", user, (unsigned)r);
    if (r) return imeError("Não foi possível abrir teclado do PS4", r, error);
    openingFrames = 0;
    opened = true;
    return true;
}

NativeTextInput::Result NativeTextInput::poll(std::string& text, std::string& error) {
    if (!opened) return Pending;
    auto state = sceImeDialogGetStatus();
    if (state == ORBIS_DIALOG_STATUS_RUNNING) {
        openingFrames = 0;
        return Pending;
    }
    if (state == ORBIS_DIALOG_STATUS_NONE && ++openingFrames <= 120) return Pending;
    OrbisDialogResult result = {};
    int r = state == ORBIS_DIALOG_STATUS_STOPPED ? sceImeDialogGetResult(&result) : -1;
    sceImeDialogTerm();
    opened = false;
    if (r) {
        imeError("Não foi possível ler resultado do teclado", r, error);
        return Failed;
    }
    if (result.endstatus != ORBIS_DIALOG_OK) return Cancelled;
    buffer[128] = 0;
    text = imeDecode(buffer, 128);
    return Accepted;
}
