# Loja Doodle — servidor

A loja é um **servidor de arquivos estáticos**. Não há aplicação, banco de dados nem login: o console só
faz `GET`. Serve em `python -m http.server`, nginx, Apache, GitHub Pages, S3 — o que estiver à mão.

## Publicar e servir

```powershell
.\store-backend\publicar.ps1
python -m http.server 8080 --directory store-backend\loja
```

O primeiro comando copia os jogos de `games/` para `store-backend/loja/` (fora do controle de versão),
**compila cada um para `jogo.doobc` e tira os `.doo`** — a loja entrega o jogo sem o código-fonte — e
gera o `catalogo.txt`. Precisa do simulador compilado (usa `build\Release\doodle.exe --build`). Para
publicar com o código, use `publicar.ps1 -Fonte`. O segundo põe a pasta no ar. No console, a loja aparece na terceira coluna do menu:
**A** instala (ou abre, se já estiver instalado) e **X** atualiza a lista.

O endereço fica salvo em `saves/sistema.sav`, na chave `loja`, e o padrão é `http://localhost:8080`.

## Assinatura

Crie o par de chaves uma vez:

```powershell
build\Release\doodle.exe --keygen store-backend\chave.priv loja.pub
```

A partir daí o `publicar.ps1` assina cada jogo (`jogo.sig` junto do `jogo.doobc`). O `loja.pub` fica na
raiz do console, e ele passa a **recusar** pacote que não bata com essa chave — adulterado no caminho ou
vindo de outra loja. Sem `loja.pub`, o console instala sem conferir.

A chave privada não entra no controle de versão (está no `.gitignore`). Perdeu, gera outro par e distribui
o `loja.pub` novo.

## O que o console pede

| Pedido | Resposta |
|---|---|
| `GET /catalogo.txt` | A lista de jogos (formato abaixo) |
| `GET /<id>/<arquivo>` | Cada arquivo do jogo: `jogo.doobc`, `jogo.sig` e os recursos |

## Formato do catálogo

Texto UTF-8 sem BOM, `chave<TAB>valor`, um bloco por jogo separado por linha em branco — o mesmo formato
dos saves do console:

```
jogo	quadrado
titulo	Quadrado
info	O primeiro teste do console
arquivo	jogo.doobc	44107
arquivo	icon.png	3120
```

`titulo` e `info` saem das linhas `titulo:` e `descricao:` do `info.txt` do jogo. Cada `arquivo` traz o
caminho relativo e o tamanho em bytes (o tamanho só serve para a barra de progresso). O jogo precisa ter
`jogo.doobc` (ou `main.doo`, se publicado com `-Fonte`); sem nenhum dos dois o console recusa o pacote.

## O que o console recusa

- Nome de jogo ou de arquivo com `..`, barra invertida, dois-pontos ou espaço — nada escapa de `games/<id>/`
- Arquivo acima de 64 MB ou catálogo acima de 1 MB
- Resposta que não for `200`

O download vai para `games/<id>.parcial/` e só vira `games/<id>/` quando termina inteiro, então uma queda de
rede não deixa jogo pela metade. O que veio da loja ganha um arquivo `.loja` na pasta, e **só o que tem essa
marca pode ser desinstalado pelo menu** — um jogo que você escreveu à mão nunca é apagado pela loja.
