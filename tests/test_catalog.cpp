#include "../src/ui.h"
#include <cassert>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    mkdir("build/test-data", 0755);
    assert(normalizeServerUrl("192.168.1.2:8765/") == "http://192.168.1.2:8765");
    assert(normalizeServerUrl("https://hb.example.org/") == "https://hb.example.org");
    assert(normalizeServerUrl("ftp://invalid") == "");
    assert(resolveServerUrl("http://192.168.1.2:8765", "/covers/1.jpg") == "http://192.168.1.2:8765/covers/1.jpg");
    assert(resolveServerUrl("http://192.168.1.2:8765", "https://cdn.example.org/a.pkg") == "https://cdn.example.org/a.pkg");
    assert(endsWithJson("http://192.168.1.2/hb/manifest.json?v=1"));
    assert(!endsWithJson("http://192.168.1.2/hb/app.pkg"));
    assert(serverBaseFromCatalogUrl("http://192.168.1.2/hb/catalog.json") == "http://192.168.1.2/hb");

    std::string resolvedPkg, manifestCid;
    uint64_t manifestSize = 0;
    std::string mErr;
    assert(extractPkgUrlFromJson(R"({"packages":[{"url":"/files/app.pkg"}],"content_id":"IV0000-APOL00004_00-APOLLOSAVETOOL04","size_bytes":4096})",
                                 "http://192.168.1.2/hb", resolvedPkg, manifestCid, manifestSize, mErr));
    assert(resolvedPkg == "http://192.168.1.2/hb/files/app.pkg" && manifestSize == 4096);

    manifestSize = 0;
    assert(extractPkgUrlFromJson(R"({"originalFileSize":63766528,"numberOfSplitFiles":1,"pieces":[{"url":"https://example.org/hb.pkg","fileOffset":0,"fileSize":63766528}]})",
                                 "http://192.168.1.2", resolvedPkg, manifestCid, manifestSize, mErr));
    assert(resolvedPkg == "https://example.org/hb.pkg" && manifestSize == 63766528);

    CatalogData arrayCat;
    assert(parseCatalogJson(R"([{"name":"Porte HB","url":"/p.pkg","cover":"/c.jpg","genre":"Jogos Homebrew"}])",
                            "http://192.168.1.2", arrayCat, mErr));
    assert(arrayCat.items.size() == 1 && arrayCat.categories.size() == 1 && arrayCat.categories[0].name == "Jogos Homebrew");

    std::string json = R"({
        "store_name": "HB_Store",
        "store_subtitle": "Catálogo Homebrew",
        "updated_at": 1763601900,
        "categories": [
            {"id": 1, "name": "Aplicativos", "slug": "apps"},
            {"id": 2, "name": "Emuladores", "slug": "emulators"}
        ],
        "items": [
            {
                "id": 10,
                "title": "Apollo Save Tool",
                "category": "Aplicativos",
                "category_id": 1,
                "version": "2.1.0",
                "author": "bucanero",
                "description": "Ferramenta \u00e9 open-source",
                "size": "18.4 MB",
                "size_bytes": 19293798,
                "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04",
                "content_type": 26,
                "content_flags": 0,
                "pkg_url": "/pkgs/apollo.pkg",
                "cover_url": "/covers/10.jpg",
                "updated_at": 1763601900
            },
            {
                "id": 11,
                "title": "RetroArch PS4",
                "category": "Emuladores",
                "category_id": 2,
                "version": "1.19",
                "author": "libretro",
                "description": "Emulador multi-sistema",
                "size": "120.0 MB",
                "size_bytes": 125829120,
                "content_id": "IV0000-RARCH0001_00-RETROARCHPS40000",
                "pkg_url": "http://192.168.1.2:8765/pkgs/retro.pkg",
                "cover_url": ""
            }
        ]
    })";

    CatalogData cat;
    std::string err;
    assert(parseCatalogJson(json, "http://192.168.1.2:8765", cat, err));
    assert(cat.storeName == "HB_Store");
    assert(cat.categories.size() == 2);
    assert(cat.items.size() == 2);
    assert(cat.items[0].url == "http://192.168.1.2:8765/pkgs/apollo.pkg");
    assert(cat.items[0].image == "http://192.168.1.2:8765/covers/10.jpg");
    assert(cat.items[0].description == "Ferramenta é open-source");

    auto round = loadCatalog(serialize(cat.items));
    assert(round.size() == 2 && round[0].title == "Apollo Save Tool" && round[1].category == "Emuladores");

    auto catsRound = loadCategories(serializeCategories(cat.categories));
    assert(catsRound.size() == 2 && catsRound[1].name == "Emuladores");

    UI ui("assets", "build/test-data");
    ui.categories = cat.categories;
    ui.games = cat.items;
    ui.filter();
    assert(ui.visible.size() == 2);

    // Filtrar por categoria 2 (Emuladores -> tab index 3)
    ui.action(TAB_NEXT); // Favoritos
    assert(ui.tab == 1 && ui.visible.empty());
    ui.action(TAB_NEXT); // Aplicativos
    assert(ui.tab == 2 && ui.visible.size() == 1 && ui.visible[0] == 0);
    ui.action(TAB_NEXT); // Emuladores
    assert(ui.tab == 3 && ui.visible.size() == 1 && ui.games[ui.visible[0]].title == "RetroArch PS4");
    ui.action(TAB_PREV);
    ui.action(TAB_PREV);
    ui.action(TAB_PREV);
    assert(ui.tab == 0 && ui.visible.size() == 2);

    // Favoritar e buscar
    ui.action(FAVORITE);
    assert(ui.favorites.size() == 1);
    ui.action(SEARCH);
    assert(ui.keyboard);
    ui.key = 17; // 'r'
    ui.action(CONFIRM);
    ui.key = 44; // OK
    ui.action(CONFIRM);
    assert(!ui.keyboard && ui.query == "r");
    ui.action(BACK);
    assert(ui.query.empty());

    // Detalhes -> Instalar em Background (Notificações) e Local (mantém detalhes aberto até CÍRCULO)
    ui.action(CONFIRM);
    assert(ui.detail);
    ui.action(CONFIRM);
    assert(ui.detail && ui.installBgRequest && ui.installItem.id == ui.current()->id);
    ui.installBgRequest = false;
    ui.action(LOCAL_INSTALL);
    assert(ui.detail && ui.installLocalRequest);
    ui.installLocalRequest = false;
    ui.action(BINLOADER_INSTALL);
    assert(ui.detail && ui.installBinLoaderRequest);
    ui.installBinLoaderRequest = false;
    ui.action(BACK);
    assert(!ui.detail);

    // Servidor config
    ui.action(SERVER_CONFIG);
    assert(ui.serverPicker);
    ui.action(DOWN);
    ui.action(CONFIRM);
    assert(ui.serverSyncRequest && !ui.serverPicker);

    // Aba Notificações
    ui.action(DOWNLOADS);
    assert(ui.tab == ui.downloadsTab());
    ui.draw();
    assert(ui.c.pixels.size() == 1920 * 1080);

    // Testar catálogo com múltiplos links e partes divididas
    CatalogData multiCat;
    assert(parseCatalogJson(R"({
        "items": [
            {
                "id": 20,
                "title": "App Multi",
                "category": "Aplicativos",
                "pkg_url": "/pkgs/base.pkg",
                "size_bytes": 4096,
                "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04",
                "use_binloader": true,
                "links": [
                    {"label": "Base", "url": "/pkgs/base.pkg", "size": "4 KB", "size_bytes": 4096, "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04", "use_binloader": false},
                    {"label": "Update v1.05", "url": "/pkgs/update.pkg", "size": "2 KB", "size_bytes": 2048, "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04", "content_type": 27, "use_binloader": true},
                    {"label": "Arquivo Direto", "url": "/files/goldhen.bin", "size": "512 B", "size_bytes": 512, "direct_download": true, "direct_path": "/data/pkg"}
                ]
            },
            {
                "id": 21,
                "title": "App Dividido",
                "category": "Aplicativos",
                "pkg_url": "/pkgs/p0.pkg",
                "split_urls": ["/pkgs/p0.pkg", "/pkgs/p1.pkg"]
            },
            {
                "id": 23,
                "title": "GoldHEN Direct",
                "category": "Aplicativos",
                "pkg_url": "/files/goldhen.bin",
                "direct_download": true,
                "direct_path": "/data/"
            }
        ]
    })", "http://192.168.1.2:8765", multiCat, err));
    assert(multiCat.items.size() == 3);
    assert(multiCat.items[0].links.size() == 3);
    assert(multiCat.items[1].splitUrls.size() == 2);
    assert(multiCat.items[2].directDownload && multiCat.items[2].directPath == "/data/");

    auto multiRound = loadCatalog(serialize(multiCat.items));
    assert(multiRound.size() == 3);
    assert(multiRound[0].useBinloader);
    assert(!multiRound[0].links[0].useBinloader);
    assert(multiRound[0].links.size() == 3 && multiRound[0].links[1].label == "Update v1.05" && multiRound[0].links[1].useBinloader);
    assert(multiRound[0].links[2].directDownload && multiRound[0].links[2].directPath == "/data/pkg");
    assert(multiRound[1].splitUrls.size() == 2);
    assert(multiRound[2].directDownload && multiRound[2].directPath == "/data/");

    ui.tab = 0;
    ui.games = multiRound;
    ui.filter();
    int idxMulti = -1;
    for (size_t vi = 0; vi < ui.visible.size(); ++vi) {
        if (ui.games[ui.visible[vi]].id == 20) { idxMulti = int(vi); break; }
    }
    assert(idxMulti >= 0);
    ui.selected = idxMulti;
    ui.action(CONFIRM); // abre detalhes de App Multi
    assert(ui.detail && ui.detailLink == 0);
    ui.action(DOWN); // seleciona Update v1.05
    assert(ui.detailLink == 1);
    ui.action(CONFIRM);
    assert(ui.installBgRequest && ui.installItem.url == "http://192.168.1.2:8765/pkgs/update.pkg" && ui.installItem.useBinloader);
    ui.installBgRequest = false;
    ui.action(DOWN); // seleciona Arquivo Direto
    assert(ui.detailLink == 2);
    ui.action(CONFIRM); // apenas aperta X no app usando o caminho configurado no painel
    assert(ui.detail && ui.installBgRequest && ui.installItem.directDownload && ui.installItem.directPath == "/data/pkg");
    ui.installBgRequest = false;
    ui.draw();
    ui.action(DOWN); // seleciona "Instalar TODOS"
    assert(ui.detailLink == 3);
    ui.action(CONFIRM);
    assert(ui.installBgRequest && ui.installBatch.size() == 3);
    ui.draw();
    ui.action(BACK);

    // PKG dividido mantém um único botão (sem lista de links separados)
    int idxSplit = -1;
    for (size_t vi = 0; vi < ui.visible.size(); ++vi) {
        if (ui.games[ui.visible[vi]].id == 21) { idxSplit = int(vi); break; }
    }
    assert(idxSplit >= 0);
    ui.selected = idxSplit;
    ui.action(CONFIRM);
    assert(ui.detail && ui.detailLink == 0);
    ui.action(DOWN);
    assert(ui.detailLink == 0);
    ui.draw();
    ui.action(BACK);

    // Muitos links (rolagem para cima/baixo + PAGE_NEXT/PAGE_PREV)
    Game gMany = multiRound[0];
    gMany.id = 22;
    gMany.links.clear();
    for (int i = 0; i < 10; ++i) {
        GameLink lk;
        lk.label = "DLC " + std::to_string(i + 1);
        lk.url = "http://192.168.1.2:8765/pkgs/dlc" + std::to_string(i + 1) + ".pkg";
        lk.size = "10 MB";
        gMany.links.push_back(lk);
    }
    ui.games = {gMany};
    ui.filter();
    ui.selected = 0;
    ui.action(CONFIRM);
    assert(ui.detail && ui.detailLink == 0);
    ui.action(PAGE_NEXT);
    assert(ui.detailLink == 4);
    ui.draw();
    ui.action(PAGE_NEXT);
    assert(ui.detailLink == 8);
    ui.draw();
    ui.action(PAGE_PREV);
    assert(ui.detailLink == 4);
    ui.action(UP);
    assert(ui.detailLink == 3);
    ui.draw();
    ui.action(BACK);

    // Spinner girando e porcentagem durante downloads (Detalhes, Aba Notificações e Grid)
    assert(!ui.hasSpinnerActivity());
    InstallPackage tActive;
    tActive.id = 101;
    tActive.itemId = 23;
    tActive.url = "http://192.168.1.2:8765/files/goldhen.bin";
    tActive.name = "GoldHEN Direct";
    tActive.mode = InstallMode::DirectSave;
    tActive.path = "/data/goldhen.bin";
    tActive.bytes = 1000;
    tActive.doneBytes = 420;
    tActive.percent = 42;
    tActive.state = InstallState::Downloading;
    assert(UI::isTaskActive(tActive));
    assert(UI::taskDisplayPercent(tActive) == 42);
    assert(UI::formatPercentText(42) == "42%");
    InstallPackage tUnknown = tActive;
    tUnknown.percent = -1;
    tUnknown.bytes = 0;
    tUnknown.doneBytes = 128;
    assert(UI::taskDisplayPercent(tUnknown) == -1);
    assert(UI::formatPercentText(-1) == "--%");
    tUnknown.doneBytes = 0;
    assert(UI::taskDisplayPercent(tUnknown) == 0);

    ui.games = multiRound;
    ui.filter();

    // Tarefas que NÃO são Download Direto não exibem spinner nem porcentagem na loja
    InstallPackage tNonDirect = tActive;
    tNonDirect.id = 100;
    tNonDirect.itemId = 20;
    tNonDirect.mode = InstallMode::RemoteBgft;
    tNonDirect.state = InstallState::Installing;
    tNonDirect.percent = 55;
    ui.tasks = {tNonDirect};
    assert(!ui.hasSpinnerActivity());
    assert(ui.primaryActiveDirectTask() == nullptr);
    assert(ui.findDirectTaskForItem(20) == nullptr);
    ui.draw();

    ui.tasks = {tActive};
    assert(ui.hasSpinnerActivity());
    assert(ui.primaryActiveDirectTask() != nullptr);
    int idxDirect = -1;
    for (size_t vi = 0; vi < ui.visible.size(); ++vi) {
        if (ui.games[ui.visible[vi]].id == 23) { idxDirect = int(vi); break; }
    }
    assert(idxDirect >= 0);
    ui.selected = idxDirect;
    ui.action(CONFIRM); // abre detalhes de GoldHEN Direct com download ativo
    assert(ui.detail);
    ui.animFrame = 0;
    ui.draw();
    std::vector<uint32_t> spinSnap0;
    for (int py = 780; py <= 836; ++py) {
        for (int px = 400; px <= 456; ++px) {
            spinSnap0.push_back(ui.c.pixels[py * Canvas::W + px]);
        }
    }
    ui.animFrame = 15;
    ui.draw();
    bool spinnerChanged = false;
    size_t spIdx = 0;
    for (int py = 780; py <= 836 && !spinnerChanged; ++py) {
        for (int px = 400; px <= 456; ++px) {
            if (ui.c.pixels[py * Canvas::W + px] != spinSnap0[spIdx++]) {
                spinnerChanged = true;
                break;
            }
        }
    }
    assert(spinnerChanged);
    ui.action(BACK);

    // Renderizar Aba Notificações com estados variados (Ativo, Concluído, Pausado, Erro)
    InstallPackage tDone = tActive;
    tDone.id = 102; tDone.state = InstallState::Complete; tDone.percent = 100; tDone.doneBytes = 1000;
    assert(UI::taskDisplayPercent(tDone) == 100);
    InstallPackage tPaused = tActive;
    tPaused.id = 103; tPaused.state = InstallState::Paused; tPaused.percent = 65;
    InstallPackage tErr = tActive;
    tErr.id = 104; tErr.state = InstallState::Error; tErr.note = "Falha de rede";
    ui.tasks = {tActive, tDone, tPaused, tErr};
    ui.tab = ui.downloadsTab();
    ui.animFrame = 22;
    ui.draw();
    assert(ui.c.pixels.size() == 1920 * 1080);

    ui.tab = 0;
    ui.tasks.clear();
    ui.syncing = true;
    assert(ui.hasSpinnerActivity());
    ui.draw();
    ui.syncing = false;
    assert(!ui.hasSpinnerActivity());

    // Gerenciador / Exclusão de arquivos em caminho pré-selecionado (/data/...)
    char fdir[] = "/tmp/hbstore-uifiles-XXXXXX";
    assert(mkdtemp(fdir));
    std::string fdirStr = fdir;
    mkdir((fdirStr + "/subignore").c_str(), 0755);
    assert(writeFile(fdirStr + "/goldhen.bin", std::string(64, 'A')));
    assert(writeFile(fdirStr + "/partial.pkg.part", std::string(16, 'B')));

    Game gFile = multiRound[2];
    gFile.id = 23;
    gFile.directDownload = true;
    gFile.directPath = fdirStr;
    gFile.url = "http://192.168.1.2:8765/files/goldhen.bin";
    ui.games = {gFile};
    ui.filter();
    ui.selected = 0;
    ui.action(CONFIRM); // abre detalhes de item direto cujo arquivo já existe no caminho configurado
    assert(ui.detail && ui.detailDirectExists && ui.detailDirectPath == fdirStr + "/goldhen.bin" && ui.detailDirectSize == 64);
    ui.action(LOCAL_INSTALL); // L3 em item direto existente abre confirmação de exclusão
    assert(ui.deleteConfirm && ui.deleteTargetPath == fdirStr + "/goldhen.bin" && !ui.installLocalRequest);
    ui.draw();
    ui.action(BACK);
    assert(!ui.deleteConfirm);

    ui.action(SORT); // TOUCHPAD em detalhes abre gerenciador da pasta pré-selecionada
    assert(ui.filesModal && !ui.filesDirs.empty() && ui.filesDirs[0] == fdirStr);
    assert(ui.filesEntries.size() == 1 && ui.filesEntries[0].name == "goldhen.bin");
    ui.draw();
    ui.action(CONFIRM); // X seleciona arquivo -> confirmação de exclusão
    assert(ui.deleteConfirm && ui.deleteTargetPath == fdirStr + "/goldhen.bin");
    ui.action(CONFIRM); // X confirma -> sinaliza execução
    assert(!ui.deleteConfirm && ui.deleteExecuteRequest);
    ui.deleteExecuteRequest = false;
    ui.action(BACK); // fecha filesModal
    ui.action(BACK); // fecha detail
    assert(!ui.filesModal && !ui.detail);

    unlink((fdirStr + "/goldhen.bin").c_str());
    unlink((fdirStr + "/partial.pkg.part").c_str());
    rmdir((fdirStr + "/subignore").c_str());
    rmdir(fdirStr.c_str());

    unlink("build/test-data/favorites.txt");
    rmdir("build/test-data");

    // Testar helpers JSON e sessão de autenticação do aplicativo
    AppAuthInfo authInfo;
    assert(parseAppAuthInfoJson(R"({
        "login_required": true,
        "authenticated": false,
        "store_name": "WILLGAME Store",
        "store_subtitle": "Painel VIP",
        "user": {"username": "cliente01", "display_name": "Cliente Um"}
    })", authInfo));
    assert(authInfo.loginRequired && !authInfo.authenticated);
    assert(authInfo.storeName == "WILLGAME Store" && authInfo.username == "cliente01" && authInfo.displayName == "Cliente Um");

    AppSession sess;
    std::string loginParseErr;
    assert(parseAppLoginJson(R"({"ok":true,"token":"tok_abc123","user":{"username":"u1","display_name":"Usuario 1"}})", sess, loginParseErr));
    assert(sess.token == "tok_abc123" && sess.username == "u1" && sess.displayName == "Usuario 1");
    sess.serverUrl = "http://192.168.1.2:8765";
    AppSession sessRound = loadAppSession(serializeAppSession(sess));
    assert(sessRound.serverUrl == sess.serverUrl && sessRound.token == sess.token &&
           sessRound.username == sess.username && sessRound.displayName == sess.displayName);

    assert(buildLoginJson("a\"b", "p\\w") == "{\"username\":\"a\\\"b\",\"password\":\"p\\\\w\"}");
    assert(parseApiErrorJson(R"({"error":"Usuário bloqueado"})") == "Usuário bloqueado");

    // Testar fluxo da tela de login na UI e teclado fallback com maiúsculas
    ui.loginScreen = true;
    ui.loginField = 0;
    ui.loginUser.clear();
    ui.loginPass.clear();
    ui.action(CONFIRM); // abre teclado para usuário
    assert(ui.keyboard && ui.kbTarget == UI::KbLoginUser);
    ui.action(FAVORITE); // ativa Shift (maiúsculas/símbolos)
    assert(ui.kbShift);
    ui.key = 0; // 'A'
    ui.action(CONFIRM);
    ui.key = 44; // OK
    ui.action(CONFIRM);
    assert(!ui.keyboard && ui.loginUser == "A" && ui.loginField == 1);

    ui.action(CONFIRM); // abre teclado para senha
    assert(ui.keyboard && ui.kbTarget == UI::KbLoginPass);
    ui.key = 1; // 'b'
    ui.action(CONFIRM);
    ui.key = 44; // OK
    ui.action(CONFIRM);
    assert(!ui.keyboard && ui.loginPass == "b" && ui.loginField == 2);

    ui.loginError = "Erro anterior";
    ui.draw();
    assert(ui.c.pixels.size() == 1920 * 1080);

    ui.action(CONFIRM); // botão Entrar
    assert(ui.loginSubmitRequest && ui.loginSubmitting && ui.hasSpinnerActivity());
    ui.loginSubmitRequest = false;
    ui.loginSubmitting = false;

    // Login bem-sucedido e opção de sair da conta no menu Servidor
    ui.loginScreen = false;
    ui.loggedUsername = "cliente01";
    ui.loggedDisplayName = "Cliente Um";
    ui.draw();
    ui.action(SERVER_CONFIG);
    assert(ui.serverPicker);
    ui.draw();
    ui.action(DOWN); // 1 Sincronizar
    ui.action(DOWN); // 2 Restaurar padrão
    ui.action(DOWN); // 3 Trocar usuário / Sair da conta
    ui.action(CONFIRM);
    assert(ui.logoutRequest && !ui.serverPicker);
    ui.logoutRequest = false;

    std::cout << "Catalog JSON parser, round-trip, auth flow, login screen and UI rendering passed.\n";
    return 0;
}
