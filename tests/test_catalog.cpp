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

    // Detalhes -> Instalar em Background (Notificações) e Local
    ui.action(CONFIRM);
    assert(ui.detail);
    ui.action(CONFIRM);
    assert(ui.installBgRequest && ui.installItem.id == ui.current()->id);
    ui.installBgRequest = false;
    ui.action(LOCAL_INSTALL);
    assert(ui.installLocalRequest);
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
                "links": [
                    {"label": "Base", "url": "/pkgs/base.pkg", "size": "4 KB", "size_bytes": 4096, "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04"},
                    {"label": "Update v1.05", "url": "/pkgs/update.pkg", "size": "2 KB", "size_bytes": 2048, "content_id": "IV0000-APOL00004_00-APOLLOSAVETOOL04", "content_type": 27}
                ]
            },
            {
                "id": 21,
                "title": "App Dividido",
                "category": "Aplicativos",
                "pkg_url": "/pkgs/p0.pkg",
                "split_urls": ["/pkgs/p0.pkg", "/pkgs/p1.pkg"]
            }
        ]
    })", "http://192.168.1.2:8765", multiCat, err));
    assert(multiCat.items.size() == 2);
    assert(multiCat.items[0].links.size() == 2);
    assert(multiCat.items[1].splitUrls.size() == 2);

    auto multiRound = loadCatalog(serialize(multiCat.items));
    assert(multiRound.size() == 2);
    assert(multiRound[0].links.size() == 2 && multiRound[0].links[1].label == "Update v1.05");
    assert(multiRound[1].splitUrls.size() == 2);

    ui.tab = 0;
    ui.games = multiRound;
    ui.filter();
    ui.selected = (ui.games[ui.visible[0]].id == 20) ? 0 : 1;
    ui.action(CONFIRM); // abre detalhes de App Multi
    assert(ui.detail && ui.detailLink == 0);
    ui.action(DOWN); // seleciona Update v1.05
    assert(ui.detailLink == 1);
    ui.action(CONFIRM);
    assert(ui.installBgRequest && ui.installItem.url == "http://192.168.1.2:8765/pkgs/update.pkg");
    ui.installBgRequest = false;
    ui.action(DOWN); // seleciona "Instalar TODOS"
    assert(ui.detailLink == 2);
    ui.action(CONFIRM);
    assert(ui.installBgRequest && ui.installBatch.size() == 2);
    ui.draw();
    ui.action(BACK);

    unlink("build/test-data/favorites.txt");
    rmdir("build/test-data");
    std::cout << "Catalog JSON parser, round-trip, categories, search, install actions and UI rendering passed.\n";
    return 0;
}
