# Manual do Doodle — linguagem Doo e SDK

Referência para quem faz jogos (e o firmware) do Doodle. Tudo aqui corresponde ao que está implementado
neste repositório; o que ainda não existe está em [Limites conhecidos](#15-limites-conhecidos).

**Sumário**

1. [Visão geral](#1-visão-geral)
2. [Compilar e rodar](#2-compilar-e-rodar)
3. [Estrutura de um jogo](#3-estrutura-de-um-jogo)
4. [Primeiro jogo](#4-primeiro-jogo)
5. [A linguagem Doo](#5-a-linguagem-doo)
6. [Objetos e ciclo de vida](#6-objetos-e-ciclo-de-vida)
7. [Herança](#7-herança)
8. [Componentes (`use`)](#8-componentes-use)
9. [API do SDK](#9-api-do-sdk)
10. [Prefabs do SDK](#10-prefabs-do-sdk)
11. [Shaders](#11-shaders)
12. [Saves](#12-saves)
13. [Firmware e APIs de sistema](#13-firmware-e-apis-de-sistema)
14. [Erros e depuração](#14-erros-e-depuração)
15. [Limites conhecidos](#15-limites-conhecidos)

---

## 1. Visão geral

O Doodle é um console com linguagem própria, a **Doo**. As peças:

| Peça | Pasta | O que faz |
|---|---|---|
| Compilador | `doo-compiler/` | Lê `.doo`, monta a AST e gera bytecode |
| VM | `vm-runtime/` | Executa o bytecode e chama o ciclo de vida dos objetos |
| SDK | `sdk/` | Física, leitores de `.obj`/`.wav`, saves e os **prefabs** (objetos prontos, em Doo) |
| Simulador | `simulator/` | O "hardware" virtual no Windows: vídeo (OpenGL), controle, áudio, tela de erro |
| Firmware | `firmware/` | O sistema do console (boot e menu estilo XMB), escrito em Doo |
| Jogos | `games/` | Uma pasta por jogo |

A tela do console tem **320 × 180** pixels (16:9) e roda a **60 quadros por segundo**. O jogo desenha nesses
pixels e o simulador amplia por um número inteiro de vezes, sem suavizar: o pixel do console aparece como
pixel. Esses limites são escolha de projeto, não acidente — a seção 15 lista todos.

## 2. Compilar e rodar

Pré-requisitos: Visual Studio (C++) e CMake.

```bash
cmake -B build
cmake --build build --config Release
build\Release\doodle.exe
```

| Comando | Para quê |
|---|---|
| `build\Release\doodle.exe` | Liga o console (firmware → menu → jogos) |
| `build\Release\doodle.exe --console` | Liga direto em tela cheia (modo console) |
| `build\Release\doodle.exe --fps` | Liga com o contador de quadros e escreve uma linha por segundo no terminal |
| `build\Release\doodle.exe --check` | Só compila o firmware e todos os jogos e mostra os erros |
| `build\Release\doodle.exe --build games/meu-jogo jogo.doobc` | Compila um jogo para bytecode, para distribuir sem o código (seção 3) |
| `... --keygen chave.priv loja.pub` | Cria o par de chaves da sua loja (uma vez) |
| `... --sign jogo.doobc chave.priv jogo.sig` | Assina um jogo compilado |
| `... --verify jogo.doobc jogo.sig loja.pub` | Confere uma assinatura (0 = confere) |
| `ctest --test-dir build -C Release` | Testes do compilador, da VM, da física e dos prefabs |
| `.\scripts\empacotar.ps1` | Monta `dist\Doodle` e `dist\Doodle.zip`: o console pronto para entregar |

Os `.doo` são compilados na hora em que o programa abre: depois de editar um jogo, basta abri-lo de novo
pelo menu, sem recompilar o simulador.

**Entregar o console para alguém:** `.\scripts\empacotar.ps1` monta uma pasta com o `doodle.exe`, o
firmware, os prefabs e os jogos já compilados, e zipa. Basta abrir o `doodle.exe` de dentro dela — não
precisa instalar nada, nem o runtime do Visual C++, que vai dentro do executável. O console lê firmware,
prefabs e jogos **da pasta onde o executável está**; rodando de `build\Release`, ele usa a pasta do
projeto, que é o que serve enquanto se desenvolve. Com `-Fonte`, o pacote leva os `.doo` em vez do
compilado.

**Controle** (teclado ou controle XInput):

| Doodle | Teclado | Controle |
|---|---|---|
| Direcional | Setas | D-pad ou analógico esquerdo |
| A / B / X / Y | Z / X / A / S | A / B / X / Y |
| L / R | Q / W | LB / RB |
| Analógico esquerdo | Setas | Analógico esquerdo |
| Analógico direito | I / J / K / L | Analógico direito |
| Gatilhos | E / R | LT / RT |
| Start / Select | Enter / Backspace | Start / — |
| HOME | Esc | Back |

HOME é sempre do sistema: sai do jogo e volta ao menu.

**Teclas do simulador** (não chegam ao jogo):

| Tecla | Para quê |
|---|---|
| F11 ou Alt+Enter | Alterna tela cheia (modo console) |
| F3 | Mostra/esconde os contadores: fps, tempo do quadro, pior quadro, e o orçamento (triângulos, desenhos, textura) |

A tela do console é sempre 640×480 em 4:3: em tela cheia a imagem é esticada até caber e o resto vira
tarja preta, então o jogo não precisa saber o tamanho do monitor.

## 3. Estrutura de um jogo

```
games/meu-jogo/
  main.doo        objeto raiz do jogo (obrigatório)
  Inimigo.doo     outros objetos: um objeto por arquivo
  info.txt        titulo: Meu Jogo
  icon.png        ícone do menu, 160 x 88
  pulo.wav        sons, imagens, modelos... (caminhos relativos à pasta)
```

- O nome da pasta é o **id** do jogo (usado nos saves e pelo firmware).
- O objeto de `main.doo` é a **raiz**: é criado quando o jogo abre. Se a raiz se destruir (`instance_destroy()`),
  o jogo fecha e o console volta ao menu.
- Todos os `.doo` da pasta são compilados juntos, então os objetos se enxergam pelo nome (`instance_create(Inimigo, pos)`).
- `info.txt`: a linha `titulo:` é o nome mostrado no menu (sem ela, aparece o id).

### Jogo compilado (`jogo.doobc`)

Para distribuir sem o código-fonte, compile para bytecode e entregue a pasta com `jogo.doobc` no lugar dos
`.doo` (é o que a loja faz):

```bash
build\Release\doodle.exe --build games/meu-jogo jogo.doobc
```

Uma pasta com `main.doo` roda do código; sem ele, o console lê o `jogo.doobc`. O arquivo leva os objetos já
compilados junto com os prefabs do SDK, então o jogo não quebra se o SDK mudar depois. Ele carrega as
funções do console por nome e é recusado com uma mensagem clara se:

- foi compilado para outra versão do bytecode (compile de novo);
- pede uma função que este console não tem;
- chama uma função exclusiva do firmware;
- está cortado ou adulterado — cada instrução, índice e salto é conferido antes de rodar, então um arquivo
  forjado não derruba o console.

## 4. Primeiro jogo

`games/ola/main.doo`:

```doo
object Ola

var x = 300
var cor = 0xFFCC00

function step() {
    if (button_check(btn_right)) { x += 200 * delta_time }
    if (button_check(btn_left))  { x -= 200 * delta_time }
    if (button_check_pressed(btn_a)) {
        cor = make_color_rgb(irandom(255), 200, 80)
        audio_play_tone(660, 80)
    }
}

function draw() {
    draw_clear(0x101820)
    draw_rectangle(x, 220, 40, 40, cor)
    draw_text(20, 20, "Olá, Doodle!", 28, 0xFFFFFF)
}
```

Ele aparece no menu de Jogos na próxima vez que o console ligar.

## 5. A linguagem Doo

### 5.1 Arquivo e sintaxe

- Um arquivo = um objeto: começa com `object Nome` (opcionalmente `extends Pai`).
- No nível de cima só entram `use`, `var` e `function`.
- Ponto e vírgula é opcional. Comentários: `// até o fim da linha`.
- Uma expressão só continua na linha de baixo com operador: `(` e `[` no começo de uma linha começam algo novo.

### 5.2 Tipos

| Tipo | Exemplos | Observações |
|---|---|---|
| número | `3`, `-0.5`, `0xFF8800` | Sempre ponto flutuante (não há int separado). `0x...` serve para cores |
| bool | `true`, `false` | |
| nil | `nil` | Valor de variáveis sem inicializador |
| string | `"texto"` | Escapes `\n`, `\t`, `\"`, `\\`. UTF-8 |
| array | `[1, "a", vec3()]` | Compartilhado: duas variáveis podem apontar para o mesmo array |
| vec3 | `vec3(1, 2, 3)` | Copiado ao atribuir (como um número) |
| objeto | o que `instance_create` devolve | Referência gerenciada (`ref<Object>`) a uma instância |
| struct | `{ hp: 10, "nome completo": "Ana" }` | O dicionário da GML. Compartilhado, como o array |

**Verdadeiro/falso:** `nil`, `false` e `0` são falsos; uma referência a objeto destruído também. Todo o resto é
verdadeiro (inclusive `""`, `[]`, `{}` e `vec3()`).

**Structs:** `s.hp` e `s["hp"]` leem e escrevem; escrever numa chave que não existe cria a chave, e ler uma
que não existe é erro — confira antes com `struct_exists`, ou use `struct_get`, que devolve `nil`. Não dá
para salvar um struct com `save_set`: salve os campos dele.

### 5.3 Variáveis e operadores

```doo
var vida = 100          // campo do objeto (nível de cima) ou local (dentro de função)
vida -= 10
var nome = "Doo" + 2    // + com string concatena e converte: "Doo2"
```

| Operadores | |
|---|---|
| Aritméticos | `+ - * / %` e o `-` unário |
| Comparação | `== != < <= > >=` (strings comparam em ordem alfabética) |
| Lógicos | `&& || !` (curto-circuito: devolvem o operando que decidiu) |
| Atribuição | `= += -= *= /=` e `++ --` (são comandos, não expressões) |
| Condicional | `cond ? sim : nao` |

`==` entre tipos diferentes é sempre falso (`1 == true` é falso). `i++` vale como comando (e no `for`), mas
não dentro de uma expressão: `a[i++]` não compila.

### 5.4 Controle de fluxo

```doo
if (vida <= 0) {
    instance_destroy()
} else if (vida < 20) {
    audio_play_tone(220, 100)
}

while (x < 10) { x += 1 }

for (var i = 0; i < array_length(itens); i++) {
    if (itens[i] == nil) { continue }   // pula para o próximo (o i++ ainda roda)
    if (itens[i] == "chave") { break }  // sai do laço
    show_debug_message(itens[i])
}

var texto = vida > 50 ? "bem" : "mal"
```

`break` e `continue` valem em `for`, `while` e `with`.

### 5.5 Funções

```doo
function dano(qtd, tipo) {
    vida -= qtd
    return vida
}
```

- Chamadas a funções do próprio objeto: `dano(10, "fogo")`. O número de argumentos é conferido ao compilar.
- `return` sem valor devolve `nil`. As funções podem estar em qualquer ordem no arquivo.
- Recursão até 200 níveis.

### 5.6 Arrays

```doo
var itens = []
array_push(itens, "espada")
itens[0] = "escudo"
itens[1 - 1] += "!"
show_debug_message(array_length(itens))     // 1
```

Índice fora do array é erro de execução.

### 5.7 vec3

```doo
var p = vec3(0, 1, 0)
p.y += 2                          // muda só a componente
var q = p + vec3(1, 0, 0) * 3     // vec3 + vec3, vec3 * número, número * vec3, vec3 / número, -vec3
```

### 5.8 Biblioteca padrão

Tudo é função solta, com os nomes da GML — quem vem do GameMaker já sabe escrever.

**Objetos e cena**

| Função | Descrição |
|---|---|
| `instance_create(Objeto, posição)` ou `instance_create(Objeto, x, y, z)` | Cria e devolve a referência |
| `instance_destroy()` | Marca este objeto para destruição no fim do quadro |
| `instance_exists(Tipo)` / `instance_exists(ref)` | Há alguma instância viva desse tipo / essa ainda existe |
| `instance_number(Tipo)` | Quantas vivas (descendentes contam) |
| `instance_find(Tipo, n)` | A n-ésima (a partir de 0), ou `nil` |
| `instance_nearest(x, y, z, Tipo)` | A mais perto do ponto, ou `nil` |
| `object_name(x)` | `"Player"` para objetos; senão `"número"`, `"string"`, `"array"`, `"vec3"`, `"bool"`, `"nil"` |
| `object_is(obj, Tipo)` | Verdadeiro se `obj` é `Tipo` ou estende `Tipo` |
| `show_debug_message(a, b, ...)` | Escreve no console do simulador |

**Números**

| Função | Descrição |
|---|---|
| `abs`, `sign`, `sqr`, `sqrt`, `power(x, n)`, `exp`, `ln`, `log2`, `log10` | |
| `floor`, `ceil`, `round`, `frac` | `round` arredonda meio para o par, como na GML |
| `min(...)`, `max(...)`, `mean(...)` | Aceitam vários argumentos |
| `clamp(v, min, max)`, `lerp(a, b, t)` | |
| `sin`, `cos`, `tan`, `arcsin`, `arccos`, `arctan`, `arctan2(y, x)` | Radianos |
| `dsin`, `dcos`, `dtan`, `degtorad`, `radtodeg` | Versões em graus |
| `pi` | Constante |

**Sorteio**

| Função | Descrição |
|---|---|
| `random(n)` | 0 (incluso) a `n` (excluso) |
| `random_range(a, b)` | Entre `a` e `b` |
| `irandom(n)` | Inteiro de 0 a `n`, **incluindo** o `n` |
| `irandom_range(a, b)` | Inteiro entre `a` e `b` |
| `choose(a, b, c, ...)` | Um dos valores, sorteado |
| `random_set_seed(n)` | Fixa a semente (útil para mundo gerado) |

**Geometria** — ângulos em graus, 0 = direita, crescendo no sentido anti-horário

| Função | Descrição |
|---|---|
| `point_distance(x1, y1, x2, y2)` | |
| `point_distance_3d(x1, y1, z1, x2, y2, z2)` | |
| `point_direction(x1, y1, x2, y2)` | Ângulo de um ponto ao outro, 0..360 |
| `lengthdir_x(dist, dir)` / `lengthdir_y(dist, dir)` | Componentes de um vetor em ângulo |
| `angle_difference(a, b)` | Caminho mais curto entre dois ângulos, -180..180 |

**Texto** — as posições começam em **1** e contam **caracteres**, não bytes, como na GML: em `"ação"`,
`string_length` é 4 e `string_char_at(s, 3)` é `"ç"`

| Função | Descrição |
|---|---|
| `string(valor)` / `real(texto)` | Converte de e para número |
| `string_length(s)` | |
| `string_upper(s)` / `string_lower(s)` | |
| `string_char_at(s, pos)` | |
| `string_copy(s, começo, quantidade)` | Pedaço do texto |
| `string_delete(s, começo, quantidade)` / `string_insert(novo, s, pos)` | |
| `string_pos(pedaço, s)` | Posição do pedaço, ou 0 se não achar |
| `string_repeat(s, n)` / `string_replace_all(s, pedaço, novo)` | |

**Arrays** — começam em **0**

| Função | Descrição |
|---|---|
| `array_create(n, valor = 0)` | |
| `array_length(a)` | |
| `array_push(a, valor)` / `array_pop(a)` | Fim do array |
| `array_insert(a, pos, valor)` / `array_delete(a, pos, quantidade = 1)` | |

**Structs**

| Função | Descrição |
|---|---|
| `struct_exists(s, chave)` | A chave existe |
| `struct_get(s, chave)` | O valor, ou `nil` se não existir |
| `struct_set(s, chave, valor)` / `struct_remove(s, chave)` | |
| `struct_get_names(s)` | Array com as chaves, em ordem alfabética |

**Outros**

| Função | Descrição |
|---|---|
| `vec3(x, y, z)` / `vec3()` | Cria um vetor (o segundo é zero) |
| `make_color_rgb(r, g, b)` | Cor `0xRRGGBB` a partir de 0..255 |

## 6. Objetos e ciclo de vida

### 6.1 Funções chamadas pelo console

| Função | Quando roda |
|---|---|
| `create()` | Uma vez, logo depois de o objeto ser criado (e depois de `instance_create` pôr a posição) |
| `step()` | Todo quadro: lógica e controle |
| `draw()` | Todo quadro, depois da física: desenho |
| `collision(other)` | Todo quadro enquanto encosta em outro objeto (física) |
| `alarm0()` … `alarm7()` | Quando o `alarm` correspondente chega ao fim |
| `destroy()` | Uma vez, quando o objeto é destruído |

Ordem de cada quadro: alarmes → `step` de todos → física (e `collision`) → `draw` de todos → `destroy` de quem morreu.
Os objetos rodam na ordem em que foram criados (a raiz primeiro). Todas as funções são opcionais.

Desenhe no `draw` e declare câmera e luzes no `step`, assim tudo o que é desenhado no quadro já usa a
câmera e as luzes daquele quadro. Desenhos 2D ficam sempre por cima do 3D.

### 6.2 Criar e conversar com objetos

```doo
var inimigo = instance_create(Inimigo, vec3(3, 0, 5))   // cria já com position; create() roda aqui
inimigo.vida -= 10                            // lê e altera campos de outro objeto
inimigo.tomar_dano(10)                        // chama funções de outro objeto
if (inimigo) { show_debug_message("ainda vivo") }          // referência fica falsa depois de destruído
if (object_name(other) == Player) { ... }            // o tipo exato
if (object_is(other, BasicCharacterController)) { ... }  // o tipo ou algum descendente
```

- `instance_create(Objeto, posição)`: a posição só é aplicada se o objeto tiver um campo `position`.
- Alterar membro (`a.b = ...`) só funciona quando `a` é uma variável; `lista[i].vida = 0` não compila
  (use `var e = lista[i]` e depois `e.vida = 0`).
- Usar um objeto já destruído é erro de execução.
- `self` é a referência para o próprio objeto: `alvo.persegue(self)`.

### 6.3 `x`, `y`, `z`

Quem tem `position` (todo objeto com componente) escreve direto nos eixos, como na GML:

```doo
x += 3 * delta_time      // o mesmo que position.x += 3 * delta_time
y = 0
if (z > 10) { z = -10 }
```

Vale também em referência para outro objeto: `inimigo.x`, `other.z += 1`.

`position` continua existindo para contas com vetor inteiro (`position + forward() * 2`). Se o objeto
declarar seu próprio `var x`, ele vence — é o caso dos prefabs de UI, onde `x` é a posição na tela.

### 6.4 Alarmes

Todo objeto tem `alarm[0]` até `alarm[7]`, contados em **quadros** (o console roda a 60 por segundo).
Ao chegar ao fim, o console chama a função `alarm0()` … `alarm7()` correspondente:

```doo
function create() {
    alarm[0] = 60          // daqui a 1 segundo
}

function alarm0() {
    atirar()
    alarm[0] = 30          // e de novo a cada meio segundo
}
```

`-1` é o valor de desligado, e é assim que eles começam. Alarme só conta com o jogo andando: quem pausa
com `time_set_scale(0)` também segura os alarmes.

### 6.5 `with`, `self` e `other`

`with` roda um bloco **como** outro objeto — igual à GML. Dentro dele, os campos e as funções são do
alvo; os `var` da função de fora continuam visíveis; e `other` é quem abriu o `with`:

```doo
function explodir() {
    var dano = 30
    with (Inimigo) {                  // todos os Inimigo vivos (e os que estendem Inimigo)
        if (point_distance(x, z, other.x, other.z) < 5) {
            hp -= dano                // hp do inimigo; dano é o local de explodir()
            other.acertos += 1        // campo de quem chamou
        }
    }
}

function limpar() {
    with (Moeda) { instance_destroy() }   // instance_destroy() dentro do with destrói o alvo
}
```

O alvo pode ser um tipo, uma referência (`with (alvo) { ... }`) ou um array de referências. A lista é
tirada na entrada do `with`: quem nascer dentro do bloco não entra na volta, e quem for destruído antes da
sua vez é pulado. `super` não funciona lá dentro.

## 7. Herança

```doo
// Heroi.doo
object Heroi extends BasicCharacterController

var jump_sound = "pulo.wav"   // muda o valor inicial de um campo do pai
var moedas = 0                // campo novo

function step() {
    super.step()            // a versão do pai
    if (position.y < -20) { position = vec3(0, 2, 0) }
}
```

- Herança simples; o pai fica em outro arquivo do mesmo jogo ou é um prefab do SDK.
- O filho herda campos, componentes (`use`) e funções. Os inicializadores rodam do pai para o filho, então o
  valor do filho vence.
- Toda chamada é "virtual": o código do pai que chama `nome()` usa a versão do filho, se houver.
- `super.f(args)` chama a versão de `f` do ancestral mais próximo que a define.
- Uma função que substitui outra precisa ter o mesmo número de parâmetros.
- Erros dentro de código herdado apontam o arquivo do pai.

## 8. Componentes (`use`)

`use Nome` no topo do objeto liga um componente: ele acrescenta campos (com valores padrão), a menos que o
objeto (ou um ancestral) já declare o campo com `var`. Todos acrescentam `position` (vec3).

| Componente | Campos (padrão) | Efeito |
|---|---|---|
| `BoxCollider` | `size` vec3(1, 1, 1), `rotation` vec3(), `trigger` false | Caixa centrada em `position`, girada por `rotation` (graus, mesma ordem do `draw_mesh`) |
| `SphereCollider` | `radius` 0.5, `trigger` false | Esfera |
| `CapsuleCollider` | `radius` 0.5, `height` 2, `rotation` vec3(), `trigger` false | Cápsula, em pé ou deitada pelo `rotation` |
| `Rigidbody` | `velocity` vec3(), `gravity` 20, `grounded` false, `slope_limit` 45, `mass` 1 | A física move o objeto e o empurra para fora do que é sólido |
| `TerrainCollider` | `size` vec3(10, 1, 10), `heights` nil | Chão com relevo (veja o prefab `Terrain`) |
| `AudioSource` | `sound` "", `volume` 1, `loop` false, `range` 20 | Som no mundo (veja `audio_source_play`) |

Física:

- Só objetos com `Rigidbody` se movem; eles são empurrados para fora de colisores sólidos e do terreno.
  `grounded` fica verdadeiro quando estão apoiados em algo.
- `trigger = true`: não bloqueia, só gera `collision` (moedas, zonas).
- Dois objetos com `Rigidbody` se empurram, cada um cedendo na medida da sua `mass`: o dobro de massa sai
  metade do lugar. `mass = 0` não sai nenhum, o que serve para plataforma e porta que empurram quem encosta.
  É uma passada por quadro, então uma pilha alta se acomoda em alguns quadros.
- `collision(other)` é chamado nos dois lados de cada par que se toca e que tem ao menos um `Rigidbody`.
- Caixa e cápsula giram pelo `rotation`: caixa girada vira rampa ou parede na diagonal, e cápsula girada
  vira tronco caído. Desenhe com o mesmo `rotation` do colisor, e o que se vê é o que colide.
- **Rampa ou ladeira:** até `slope_limit` graus (45 por padrão) é chão — o objeto sobe andando, fica parado
  sem escorregar e `grounded` fica verdadeiro. Mais inclinado que isso, ele escorrega e não conta como
  apoiado. Vale para caixa girada e para o terreno.

## 9. API do SDK

Argumentos com `= valor` são opcionais. Cores são números `0xRRGGBB`; `alpha` vai de 0 a 1. Caminhos de
arquivo são relativos à pasta do jogo.

### 9.1 Desenho 2D

| Função | Descrição |
|---|---|
| `draw_clear(cor)` | Pinta a tela inteira (e limpa a profundidade) |
| `draw_rectangle(x, y, w, h, cor, alpha = 1)` | Retângulo |
| `draw_gradient(x, y, w, h, cor_topo, cor_base, alpha_topo = 1, alpha_base = 1)` | Gradiente vertical |
| `draw_text(x, y, texto, tamanho, cor, alpha = 1)` | Texto (`y` = topo) |
| `string_width(texto, tamanho)` | Largura em pixels (para centralizar) |
| `draw_sprite(caminho, x, y, w, h, alpha = 1)` | PNG, JPG, GIF ou BMP; devolve `false` se o arquivo não existir. Rosa `0xFF00FF` vira transparente |

### 9.2 Desenho 3D

| Função | Descrição |
|---|---|
| `camera_set(posição, alvo, fov = 60)` | Câmera em perspectiva (vale para o quadro) |
| `draw_mesh_mix(quadroA, quadroB, mistura, posição, rotação, escala, cor, textura = "")` | Dois `.obj` da mesma malha misturados vértice a vértice (`mistura` 0..1): é assim que se anima modelo (veja o prefab `AnimatedModel`) |
| `camera_set_ortho(posição, alvo, largura)` | Câmera sem perspectiva: `largura` unidades do mundo cabem na tela de ponta a ponta (bom para visão isométrica e de cima) |
| `draw_mesh(malha, posição, rotação, escala, cor, textura = "", alpha = 1)` | Desenha uma malha. `malha` é `mesh_cube/Sphere/Cylinder/Capsule/Plane` ou um arquivo `.obj`. `rotação` em graus (vec3, ordem da Unity). `escala` é número ou vec3. `cor` tinge (0xFFFFFF mantém as cores do modelo) |
| `light_directional(direção, cor, intensidade = 1)` | Luz tipo sol (direção para onde ela aponta) |
| `light_point(posição, cor, alcance, intensidade = 1)` | Lâmpada: some suavemente até `alcance` |
| `light_spot(posição, direção, cor, alcance, ângulo = 45, intensidade = 1)` | Holofote/lanterna; `ângulo` é a abertura do cone |
| `light_ambient(cor)` | Luz que chega em todo lugar |
| `shader_set(nome)` | Shader do 3D neste quadro (veja [Shaders](#11-shaders)) |
| `draw_terrain(posição, tamanho, alturas, cor, textura = "", repetição = 1)` | Chão com relevo (use o prefab `Terrain`) |
| `heightmap_read(caminho)` | Lê uma imagem em tons de cinza: devolve linhas de alturas 0..1 |

- Primitivas em tamanho de Unity: cubo 1, esfera Ø1, cilindro e cápsula Ø1 × 2 de altura, plano 1 × 1.
- Modelos `.obj`: materiais do `.mtl` (`Kd` e `map_Kd`), polígonos de qualquer tamanho, normais calculadas se
  faltarem.
- Luzes e shader são declarados a cada quadro (no `update`). São até 8 luzes por quadro; sem nenhuma, a cena
  usa um sol padrão.
- Texturas: filtro "pixel duro" de perto, mipmaps de longe. A perspectiva é corrigida (sem o entortamento do
  PS1, a não ser com o shader `ps1`).

### 9.3 Controle

| Função | Descrição |
|---|---|
| `button_check(btn_x, jogador = 1)` | Verdadeiro enquanto o botão está apertado |
| `button_check_pressed(btn_x, jogador = 1)` | Verdadeiro só no quadro em que foi apertado |
| `stick_x(jogador = 1)` / `stick_y(jogador = 1)` | Analógico esquerdo, -1..1 (y positivo = para cima/para frente) |
| `look_x(jogador = 1)` / `look_y(jogador = 1)` | Analógico direito, -1..1 |
| `trigger_l(jogador = 1)` / `trigger_r(jogador = 1)` | Gatilhos analógicos (LT / RT), 0..1 |
| `pad_vibrate(esquerda, direita, segundos, jogador = 1)` | Liga os dois motores (0..1) por um tempo; `segundos` 0 desliga |
| `pad_connected(jogador = 1)` | Há um controle plugado para esse jogador |

Botões: `btn_up`, `btn_down`, `btn_left`, `btn_right`, `btn_a`, `btn_b`, `btn_x`, `btn_y`, `btn_l`, `btn_r`,
`btn_start`, `btn_select`, `btn_home`.

**Dois jogadores**: o controle 1 é o jogador 1 e o controle 2 é o jogador 2. O teclado vale sempre como
jogador 1, junto com o controle 1 — quem não passa o número de jogador está lendo o jogador 1.

Os analógicos já vêm com zona morta tratada: dentro dela valem 0, e fora dela o valor recomeça do 0 em vez
de pular. Sem controle plugado, as **setas** fazem o analógico esquerdo e **I/J/K/L** o direito, então um
jogo pode ler só os analógicos e funcionar nos dois. O esquerdo também continua valendo como direcional.

### 9.4 Áudio

| Função | Descrição |
|---|---|
| `audio_play_tone(hz, ms)` | Toca um tom (onda quadrada). Devolve um id |
| `audio_play_sound("arquivo", volume = 1)` | Toca um arquivo de som. Devolve um id |
| `audio_play_loop("arquivo", volume = 1)` | Toca em loop até `audio_stop_sound(id)` |
| `audio_stop_sound(id)` / `audio_stop_all()` | Para um som / todos |
| `audio_source_play()` | Com `use AudioSource`: toca `sound` na posição do objeto; o volume cai com a distância até a câmera (zero em `range`), o som sai do lado em que o objeto está na tela, e acompanha `volume` ao vivo. Devolve um id |
| `audio_source_stop()` | Para os sons deste objeto |

**Formatos:** WAV PCM de 8 ou 16 bits, e tudo o que o Windows souber decodificar — **MP3**, WMA, AAC. Ogg
não, porque o sistema não traz esse decodificador. O arquivo é decodificado inteiro na memória quando toca
pela primeira vez: MP3 economiza espaço no disco e no download da loja, não na memória (11 KB de MP3 viram
uns 0,3 MB tocando). Isso conta no orçamento de mídia (seção 15).

Os sons se misturam, até 24 ao mesmo tempo. Todos param quando o jogo fecha; um som posicional também para
quando o objeto é destruído.

### 9.5 Tempo

| Função | Descrição |
|---|---|
| `delta_time` | Segundos do último quadro, vezes a escala (0 com o jogo pausado) |
| `delta_time_real` | Segundos reais do último quadro (para UI em pausa) |
| `time_scale` / `time_set_scale(s)` | Escala do tempo: 0 = pausado, 1 = normal, 0.5 = câmera lenta. A física segue a escala |
| `clock_time()` / `clock_date()` | Hora `"14:05"` e data `"11/09"` do sistema |

### 9.6 Saves e jogos instalados

| Função | Descrição |
|---|---|
| `save_set(chave, valor)` | Guarda número, texto ou bool no save deste jogo |
| `save_get(chave, padrão)` | Lê do save deste jogo (`padrão` se não houver) |
| `game_list()` | Ids dos jogos instalados |
| `game_installed(id)` | |
| `game_title(id)` | Título do `info.txt` (ou o id) |
| `game_save_data(id)` | Save bruto de um jogo (`""` se não houver). Um jogo só lê o próprio |
| `store_available()` | Ids do catálogo da loja (vazio enquanto não carregou) |
| `store_title(id)` / `store_info(id)` / `store_size(id)` | Título, descrição e tamanho em bytes de um jogo do catálogo |
| `store_ready()` | O catálogo já chegou |
| `store_busy()` | A loja está baixando alguma coisa |
| `store_progress()` | 0..1 do download em andamento |
| `store_error()` | Mensagem do último erro da loja (`""` se deu tudo certo) |
| `store_can_uninstall(id)` | O jogo veio da loja (só esses podem ser desinstalados) |

"Instalado" é ter uma pasta com `main.doo` ou `jogo.doobc` em `games/`, tenha vindo da loja ou não. Baixar e instalar é
coisa do firmware (seção 13).

### 9.7 Física

| Função | Descrição |
|---|---|
| `terrain_height(x, z)` | Altura do terreno deste objeto (precisa de `use TerrainCollider`), ou `nil` fora dele |
| `ground_below(x, y, z)` | Altura do chão sólido mais alto **abaixo** desse ponto (terreno e colisores parados), ou `nil` se não houver nada embaixo |

O prefab `Terrain` expõe o `terrain_height` como `height_at`.

### 9.8 Sistema (só o firmware)

`system_launch(id)`, `system_volume(0..1)`, `system_delete_save(id)`: veja a seção 13. Um jogo que chame
`system_*` não compila.

## 10. Prefabs do SDK

Objetos prontos, escritos em Doo, em `sdk/prefabs/`. Todo programa os recebe; use com `instance_create` ou estenda com
`extends`. Se o jogo tiver um objeto com o mesmo nome, vale o do jogo.

### BasicCharacterController

Personagem em 3ª pessoa (como os Starter Assets da Unity): controle "tank" (esquerda/direita giram, cima/baixo
andam), A pula, câmera atrás dele. Usa `Rigidbody` e `CapsuleCollider`.

| Campo | Padrão | |
|---|---|---|
| `heading` | 0 | Direção em graus |
| `move_speed` / `back_speed` | 5 / 3 | Unidades por segundo |
| `turn_speed` | 140 | Graus por segundo |
| `jump_speed` | 7 | |
| `jump_sound` | "" | WAV opcional |
| `color` | 0xF2F2F2 | |
| `controllable` | true | false ignora o controle (menus, cenas) |
| `follow_camera` | true | Câmera atrás do personagem |
| `camera_distance` / `camera_height` | 7 / 3 | |
| `camera_yaw` / `look_speed` | 0 / 120 | A câmera que o analógico direito gira |
| `shadow` | true | Cria a sombra de mancha no chão (prefab `Shadow`) |

Funções: `forward()` (vec3 para onde olha), `control()`, `camera()`.

### ParticleSystem

```doo
var fx = instance_create(ParticleSystem, posição)
fx.color = 0xFFD23F
fx.burst(20)          // explosão; ou fx.emitting = true para um jato contínuo
```

| Campo | Padrão | |
|---|---|---|
| `color`, `size` | 0xFFFFFF, 0.15 | Cada pedaço encolhe até sumir |
| `speed`, `spread` | 4, 1 | `spread` 0 = só para cima |
| `gravity` | 9 | Negativa faz subir (fumaça) |
| `lifetime` | 0.8 | Segundos |
| `rate`, `emitting` | 30, false | Jato contínuo |
| `auto_destroy` | true | Some quando para de emitir e acaba |
| `max` | 64 | Partículas simultâneas (fixo ao criar) |

### Light

```doo
var lampada = instance_create(Light, vec3(0, 3, 0))
lampada.color = 0xFFAA55
lampada.range = 10
```

Campos: `object_name` (`lt_point` padrão, `lt_spot`, `lt_directional`), `enabled`, `color`, `intensity`,
`range`, `direction` (vec3(0, -1, 0)), `angle` (45).

### Shadow

Sombra de mancha embaixo de um objeto, como nos jogos da época: um disco escuro no chão, que encolhe e
clareia conforme o objeto sobe. Não tem forma nem direção de luz — serve para o pulo ficar legível.

```doo
var s = instance_create(Shadow, position)
s.target = self      // segue esse objeto (e some quando ele morre)
s.size = 1.1
```

Campos: `target`, `size`, `alpha`, `fade` (altura em que a sombra some de vez) e `color`. Sem `target`,
mova a `position` na mão. O `BasicCharacterController` já cria a sua: desligue com `shadow = false`.

### AnimatedModel

Animação de modelo do jeito da época: cada quadro é um `.obj` com a **mesma malha** (mesmos vértices, na
mesma ordem), e o console mistura um no outro. Sem osso, sem esqueleto — o PS1 animava personagem assim.

```doo
var bicho = instance_create(AnimatedModel, vec3(0, 1, 0))
bicho.frames = ["voo1.obj", "voo2.obj", "voo3.obj"]
bicho.fps = 8
bicho.scale = 2
bicho.rotation = vec3(0, 90, 0)
```

| Campo | |
|---|---|
| `frames` | Os `.obj` em ordem; o último volta para o primeiro |
| `fps` | Quadros por segundo da animação |
| `loop` / `playing` | Repetir; e ligar/desligar o avanço |
| `smooth` | `false` troca seco de quadro, como jogos que animavam a 10 quadros por segundo |
| `frame` | Em que ponto da animação está, com a fração entre dois quadros |
| `position`, `rotation`, `scale`, `color`, `texture` | Como no `draw_mesh` |

`play(lista, fps)` troca de animação e recomeça do zero. Estenda o prefab para dar comportamento ao bicho
(`object Passaro extends AnimatedModel`, chamando `super.step()`).

Os quadros precisam bater vértice a vértice: exporte todos do mesmo modelo, só movendo o que é para animar.
O `scripts/passaro.py` gera um exemplo de três quadros.

### Terrain

```doo
var chao = instance_create(Terrain, vec3(0, 0, 0))
chao.heightmap = "relevo.png"    // branco = alto; o topo da imagem fica do lado -z
chao.size = vec3(80, 6, 80)      // largura, altura máxima, profundidade
chao.texture = "grama.png"
chao.tiling = 16
chao.texture2 = "pedra.png"      // opcional: segunda textura...
chao.mask = "relevo.png"         // ...que aparece onde a máscara é clara
chao.build()                     // depois de ajustar os campos
var y = chao.height_at(10, -5)   // para pôr objetos no chão
```

Sem `heightmap`, é um plano. Usa `TerrainCollider`: quem tem `Rigidbody` anda por cima.

**Duas texturas:** `mask` é uma imagem em tons de cinza esticada sobre o terreno — branco mostra `texture2`,
preto mostra `texture`, e o meio mistura. Pinte uma trilha de terra numa máscara, ou use o próprio
heightmap como máscara para a segunda textura aparecer nos morros. `tiling2` repete a segunda textura
separado (0 = o mesmo `tiling`). A segunda textura custa uma passada a mais, então dobra os triângulos do
terreno no orçamento.

### UI: Canvas, Text, Image, Button, Slider, TextField, List

```doo
var menu = instance_create(Canvas)
var fundo = menu.image("", 0, 0, 320, 180)   // path "" = painel da cor `color`
fundo.color = 0x000000
fundo.alpha = 0.6
var jogar = menu.button("Jogar", 110, 70, 100, 20)
var volume = menu.slider("Volume", 90, 96, 140, 0, 100, 80)
var nome = menu.field("Nome", 90, 120, 140, "")
var fases = menu.list(90, 140, 140, 26, ["Fase 1", "Fase 2", "Fase 3"])
menu.text("Menu", 130, 40, 16)

// no step:
if (jogar.clicked) { ... }
if (volume.changed) { ... volume.value ... }
if (nome.changed) { ... nome.value ... }
if (fases.clicked) { ... fases.index ... }
```

`Canvas`:

- Desenha os elementos na ordem em que foram criados, por cima do 3D.
- Cuida do foco: o direcional leva ao elemento mais próximo naquela direção, A aperta botões e
  esquerda/direita ajustam sliders.
- Campos: `visible`, `active` (false = só mostra, bom para HUD), `sounds`, `accent` (cor do foco).
- Funções: `text`, `image`, `button`, `slider`, `field`, `list`, `add(elemento)` e `focus_on(elemento)`.

| Elemento | Campos principais |
|---|---|
| todos (`UIElement`) | `x`, `y`, `w`, `h`, `visible`, `alpha`, `color`, `accent` |
| `Text` | `text`, `size`, `align` (`"left"` / `"center"`) |
| `Image` | `path` |
| `Button` | `text`, `size`, `background`, `clicked` (vale um quadro) |
| `Slider` | `label`, `value`, `min`, `max`, `step`, `changed` (vale um quadro) |
| `TextField` | `label`, `value`, `max`, `placeholder`, `changed` (vale um quadro) |
| `List` | `items`, `index`, `row` (altura da linha), `clicked` (vale um quadro) |

**`TextField`** abre um **teclado na tela** quando você aperta A, como o do PSP: o direcional escolhe a
letra, A digita, X apaga, Y alterna maiúscula, Start confirma e B cancela (devolve o texto de antes). Tem
acento e `ç` — as funções de texto contam caracteres, então apagar tira a letra inteira.

**`List`** mostra as linhas que couberem em `h` e rola conforme você anda. Na primeira linha, subir passa o
foco para o elemento de cima; na última, descer faz o mesmo — a lista não prende o foco.

Elementos próprios: `object MeuWidget extends UIElement`, sobrescrevendo `paint(focused, t)` e, se interagir,
`focusable()`, `activate()`, `adjust(d)`, `navigate(dx, dy)` (consumir o direcional, como a lista) e
`reset()`. Para tomar o controle inteiro enquanto está aberto (como o teclado na tela), devolva `true` em
`holding()` e trate o controle em `input()`.

Menu de pausa típico: `time_set_scale(0)`, `jogador.controllable = false`, `menu.visible = true`.

## 11. Shaders

`shader_set(nome)` escolhe o shader do 3D no quadro:

| Nome | Visual |
|---|---|
| `"padrao"` | Luz por pixel (o padrão) |
| `"ps1"` | O visual do PS1 como escolha: vértices tremendo, textura afim e luz por vértice |
| `"agua"` (qualquer outro) | `agua.vert` e/ou `agua.frag` da pasta do jogo; a parte que faltar usa a padrão |

Shaders próprios são GLSL 1.20 **sem** `#version`. O console acrescenta antes:

- Uniforms: `lightCount`, `lightRange[8]`, `tex`, `useTexture`, `time`, `snapGrid`.
- Varyings: `vPos` e `vNormal` (espaço do olho), `vColor`, `vUV`.
- `vec3 lighting(vec3 p, vec3 n)`: ambiente + todas as luzes do quadro.
- O estado do OpenGL fixo, como `gl_ModelViewMatrix` e `gl_LightSource`.

Exemplo de `agua.frag`:

```glsl
void main() {
    vec4 c = vColor * (useTexture != 0 ? texture2D(tex, vUV + vec2(time * 0.05, 0.0)) : vec4(1.0));
    gl_FragColor = vec4(c.rgb * lighting(vPos, normalize(vNormal)), c.a);
}
```

Sem suporte a shaders no driver, o console usa luz por vértice e `shader_set` devolve `false`.

## 12. Saves

```doo
function create() {
    recorde = save_get("recorde", 0)
}

function fim_de_partida(pontos) {
    if (pontos > recorde) {
        recorde = pontos
        save_set("recorde", recorde)
    }
}
```

Cada jogo tem um arquivo `saves/<id>.sav` (texto, uma chave por linha). O firmware mostra "Dados salvos" no
menu e pode apagar o save pelo menu de opções (Y).

## 13. Firmware e APIs de sistema

O firmware é um programa Doo com privilégios (`firmware/main.doo`, `firmware/Xmb.doo` e `firmware/Loja.doo`):
mostra o boot, o menu estilo XMB com as categorias Configurações, Jogos e Loja, e a loja como aplicativo de
tela cheia. Só ele pode usar:

| Função | Descrição |
|---|---|
| `system_launch(id)` | Abre um jogo (o firmware fica suspenso e volta quando o jogo sai) |
| `system_volume(v)` | Volume geral do console, 0..1 |
| `system_delete_save(id)` | Apaga o save de um jogo |
| `store_set_url(endereço)` | Endereço da loja |
| `store_refresh()` | Busca o catálogo (em segundo plano) |
| `store_install(id)` | Baixa e instala um jogo do catálogo (em segundo plano) |
| `store_uninstall(id)` | Apaga um jogo que veio da loja |

As configurações do firmware (volume, tema e o endereço da loja, na chave `loja`) ficam em
`saves/sistema.sav`.

**Assinatura:** se existir um arquivo `loja.pub` na raiz do console, ele **só instala jogo compilado e
assinado** por essa chave — pacote adulterado no caminho, ou vindo de outra loja, é recusado na hora, antes
de gravar. Sem `loja.pub`, instala sem conferir (modo caseiro). A chave privada fica com quem publica e
nunca vai para o console. É ECDSA P-256, do próprio Windows.

**A loja como aplicativo** (`firmware/Loja.doo`): a coluna Loja do menu tem um item só, "Abrir a loja", que
entrega a tela inteira para ela — lista rolando à esquerda, detalhes do jogo à direita (ícone, título,
descrição, tamanho e se já está instalado), busca pelo teclado da tela e barra de progresso do download.
**A** instala ou abre, **X** atualiza o catálogo, **Y** desinstala e **B** volta ao menu. É um objeto Doo
como outro qualquer, montado com os prefabs de UI do SDK (`Canvas`, `List`, `TextField`, `Button`).

**A loja** é um servidor de arquivos estáticos: `GET /catalogo.txt` lista os jogos e `GET /<id>/<arquivo>`
baixa cada um. Ver [store-backend/README.md](../store-backend/README.md) para o formato e para publicar.
Baixar não pode travar o quadro, então roda numa thread do simulador: `store_install(id)` volta na hora e o
firmware acompanha por `store_busy()` e `store_progress()`. O download vai para `games/<id>.parcial/` e só
vira `games/<id>/` quando termina inteiro; o que veio da loja ganha um arquivo `.loja` na pasta, e só o que
tem essa marca pode ser desinstalado — um jogo escrito à mão nunca é apagado pela loja.

## 14. Erros e depuração

- **Erro de compilação ou de execução:** o console mostra uma tela vermelha com `arquivo:linha: mensagem`.
  HOME volta ao menu (se o erro foi no firmware, ele recarrega do disco).
- `doodle.exe --check`: compila tudo sem abrir janela e lista os erros.
- `show_debug_message(...)` escreve na janela de console do simulador.
- Erros comuns:

| Mensagem | Causa |
|---|---|
| `'x' não foi declarada` | Faltou `var x` (campo ou local) |
| `f() recebe N argumento(s), veio M` | Número de argumentos errado |
| `só dá pra alterar membro de uma variável` | `lista[i].x = ...`: use uma variável intermediária |
| `'system_launch' é exclusiva do firmware` | Jogos não usam `system_*` |
| `... em um objeto que já foi destruído` | Referência a um objeto que não existe mais: teste `if (ref)` antes |
| `recursão profunda demais (stack overflow)` | Mais de 200 chamadas aninhadas |

## 15. Os limites do console

### 15.1 De propósito

O Doodle é um console pequeno por decisão de projeto: a graça é fazer jogo com a régua da época. Estes
números são fixos, e o simulador mede o que o jogo gasta para você saber onde está antes de rodar no
hardware da Fase 3 (aperte **F3** para ver, ou rode com `--fps`).

| Limite | Valor | O que acontece ao passar |
|---|---|---|
| Tela | 320 × 180, 16:9 | Fixo: não há como pedir outra resolução |
| Quadros | 60 por segundo | O console espera o vsync |
| Triângulos | 30 000 por quadro | Aviso no terminal e o contador fica vermelho |
| Desenhos | 600 por quadro | Idem |
| Mídia | 8 MB de textura e som carregados | Idem |
| Luzes | 8 por quadro | A nona é recusada (`light_point` devolve `false`) |
| Vozes de áudio | 24 ao mesmo tempo, como o PS1 | A mais antiga (fora as de loop e posicionais) cede o lugar |
| Jogadores | 2 controles | O teclado conta como jogador 1 |
| Recursão | 200 chamadas | Erro de execução |
| Números | Um tipo só, ponto flutuante — como o `real` da GML | Não existe inteiro separado |

O terreno vira uma grade de no máximo 65 × 65 (8 mil triângulos) e as malhas prontas são de baixa
contagem, de propósito: esfera com 320 triângulos, cápsula com 672.

Estas outras também são escolhas, não falta de trabalho — são o que dá a cara da época:

| Escolha | Por quê |
|---|---|
| Sombra só de mancha, sem sombra projetada | É o que os jogos da época faziam, e custa um disco no chão |
| Animação de modelo por quadros-chave, sem esqueleto | O PS1 animava assim: os quadros já vêm deformados |
| Interface só de controle, sem mouse nem toque | É um console; texto se escreve no teclado da tela |
| Um tipo de número só (ponto flutuante) | É o `real` da GML |
| Sem Ogg | O Windows não traz esse decodificador, e trazer um seria dependência nova |

### 15.2 O que ainda falta

Isto não é escolha, é trabalho a fazer:

| Área | Limite atual |
|---|---|
| Física | Sem rotação por torque (o objeto só gira se o jogo girar); dois corpos girados se tocando colidem um pouco antes nas quinas |
| Render | Terreno com duas texturas no máximo |
| Áudio | Sem streaming: o som é decodificado inteiro na memória, então música longa pesa na memória; esquerda/direita só em saída estéreo |
| Controle | O teclado do PC vale só como jogador 1 |
| Loja | Servidor de arquivos estáticos: sem conta e sem pagamento (o pacote já vai assinado) |
