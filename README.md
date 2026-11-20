# Homebrew Store PS4 + Painel Web de Gerenciamento

Loja de homebrews para PS4 com **Painel Web de gerenciamento** integrado e instalação em background diretamente nas **Notificações do PS4** (via API nativa BGFT).

## O que este projeto inclui

1. **App PS4 (`HomebrewStore-PS4-*.pkg`)**
   - Sincroniza categorias, links de `.pkg`, metadados e capas diretamente do Painel Web (`GET /api/catalog.json`).
   - Navegação por **Categorias dinâmicas** (`L1` / `R1`), busca via teclado nativo do PS4 (`TRIÂNGULO`), favoritos (`QUADRADO`) e ordenação (`TOUCHPAD`).
   - **Instalação em Background nas Notificações do PS4 (`X`)**: registra o pacote na fila nativa do sistema (`sceBgftServiceDownloadRegisterTask` + `sceBgftServiceDownloadStartTask`) para que o PS4 baixe e instale em segundo plano nas Notificações.
   - **Modo alternativo (`L3` nos detalhes)**: baixa o `.pkg` dentro do app e instala localmente via `StorageEx`.
   - **Configuração de IP/URL no próprio PS4 (`L3` na loja)**: edite o endereço do servidor (padrão `http://192.168.1.2:8765`) a qualquer momento sem recompilar o PKG.

2. **Painel Web de Gerenciamento (`web-panel/`)**
   - Interface web para **adicionar, editar, ocultar e remover homebrews, links `.pkg`, capas e categorias**.
   - Aceita **links HTTP/HTTPS externos** ou **upload direto de arquivos `.pkg` e capas** pelo navegador.
   - Detecta automaticamente `Content ID`, tipo e tamanho do `.pkg` ao inspecionar o cabeçalho `\x7FCNT`.
   - Converte capas enviadas ou baixadas para JPEG compatível com o PS4 e serve arquivos `.pkg` com suporte completo a `Range` (`206 Partial Content`) e proxy HTTP opcional para links HTTPS externos.

---

## Como rodar o Painel Web

Requer **Python 3.10+** (opcionalmente `pip install pillow` para converter capas WebP/PNG automaticamente em JPEG).

### Windows
Dê um duplo clique em `web-panel/iniciar_painel.bat` ou execute:
```powershell
cd web-panel
python server.py --port 8765
```

### Linux / macOS
```bash
cd web-panel
./iniciar_painel.sh --port 8765
```

Ao iniciar, o terminal mostra o endereço da sua rede local (ex.: `http://192.168.1.2:8765`).
- **No PC/Celular**: abra o endereço no navegador para gerenciar categorias, links e capas.
- **No PS4**: abra a loja, pressione **L3** (ou **OPTIONS** para sincronizar), informe o mesmo endereço e sincronize.

---

## Como compilar o `.pkg` no GitHub

O workflow [.github/workflows/build-pkg.yml](.github/workflows/build-pkg.yml) compila automaticamente em `ubuntu-24.04`:
1. Executa os testes unitários (`make test`, incluindo testes do painel web em Python e testes C++ com ASan/UBSan).
2. Baixa o OpenOrbis PS4 Toolchain (`v0.5.4`), gera `eboot.bin` com permissões de instalador (`tools/installer-auth.hex`) e empacota `HomebrewStore-PS4-*.pkg`.
3. Valida o `.pkg` gerado (`PkgTool.Core pkg_validate` + `tools/verify_package.py`) e publica em **Actions → Artifacts** (ou em **Releases** ao enviar uma tag `v*`).

Veja detalhes em [BUILDING.md](BUILDING.md) e [TESTING.md](TESTING.md).

## Licença

Código-fonte da aplicação sob [GPL-3.0](LICENSE). Componentes e avisos de terceiros mantidos em `assets/`.
