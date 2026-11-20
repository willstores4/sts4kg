#include "ui.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

int main(int argc, char** argv) {
    UI ui("assets", "build");
    Category c1{1, "Aplicativos", "apps"}, c2{2, "Emuladores", "emulators"};
    ui.categories = {c1, c2};
    Game g;
    g.id = 1;
    g.title = "Apollo Save Tool";
    g.category = "Aplicativos";
    g.categoryId = 1;
    g.version = "2.1.0";
    g.author = "bucanero";
    g.description = "Gerenciador de saves homebrew open-source para PS4.";
    g.size = "18.4 MB";
    g.sizeBytes = 19293798;
    g.contentId = "IV0000-APOL00004_00-APOLLOSAVETOOL04";
    g.url = "http://192.168.1.2:8765/pkgs/apollo.pkg";
    ui.games.push_back(g);
    ui.filter();

    if (argc > 1 && std::string(argv[1]) == "details") ui.action(CONFIRM);
    if (argc > 1 && std::string(argv[1]) == "search") ui.action(SEARCH);
    if (argc > 1 && std::string(argv[1]) == "server") ui.action(SERVER_CONFIG);
    if (argc > 1 && std::string(argv[1]) == "downloads") {
        ui.tab = ui.downloadsTab();
        InstallPackage p;
        p.id = 1;
        p.taskId = 42;
        p.name = "Apollo Save Tool";
        p.contentId = g.contentId;
        p.bytes = g.sizeBytes;
        p.doneBytes = g.sizeBytes / 2;
        p.percent = 50;
        p.state = InstallState::Installing;
        p.note = "Baixando em Notificações do PS4";
        ui.tasks.push_back(p);
    }

    ui.draw();
    std::vector<unsigned char> rgb(1920 * 1080 * 3);
    for (size_t i = 0; i < ui.c.pixels.size(); ++i) {
        auto p = ui.c.pixels[i];
        rgb[i * 3] = p >> 16;
        rgb[i * 3 + 1] = p >> 8;
        rgb[i * 3 + 2] = p;
    }
    std::string out = argc > 1 ? std::string("build/preview-") + argv[1] + ".png" : "build/preview.png";
    return stbi_write_png(out.c_str(), 1920, 1080, 3, rgb.data(), 0) ? 0 : 1;
}
