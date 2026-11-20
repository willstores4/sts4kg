# Testar a Loja Homebrew no PS4

1. Inicie o **Painel Web** (`web-panel/server.py`) no seu PC/servidor na mesma rede do PS4.
2. No navegador do PC, adicione ao menos uma categoria/homebrew (link `.pkg` ou upload de `.pkg` + capa).
3. Instale e abra o `HomebrewStore-PS4-*.pkg` no PS4 com HEN ativo.
4. Caso o IP do painel seja diferente de `http://192.168.1.2:8765`, pressione **L3** na tela principal para digitar o endereço do seu painel.
5. Pressione **OPTIONS** para sincronizar e confirme se categorias, capas e itens aparecem.
6. Abra um homebrew (**X**) e selecione **X — Instalar em Background (Notificações PS4)**; verifique a barra de progresso tanto na aba **Notificações (R3)** quanto nas Notificações nativas do PS4.

Logs no console ficam salvos em `/data/hb-store/` (`network.log`, `installer.log`, `tasks.tsv`).
