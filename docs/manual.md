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

A tela virtual tem **640 × 480** pixels e roda a **60 quadros por segundo**. A janela pode ser redimensionada;
a imagem mantém a proporção 4:3.

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
| `ctest --test-dir build -C Release` | Testes do compilador, da VM, da física e dos prefabs |

Os `.doo` são compilados na hora em que o programa abre: depois de editar um jogo, basta abri-lo de novo
pelo menu, sem recompilar o simulador.

**Controle** (teclado ou controle XInput):

| Doodle | Teclado | Controle |
|---|---|---|
| Direcional | Setas | D-pad ou analógico esquerdo |
| A / B / X / Y | Z / X / A / S | A / B / X / Y |
| L / R | Q / W | LB / RB |
| Start / Select | Enter / Backspace | Start / — |
| HOME | Esc | Back |

HOME é sempre do sistema: sai do jogo e volta ao menu.

**Teclas do simulador** (não chegam ao jogo):

| Tecla | Para quê |
|---|---|
| F11 ou Alt+Enter | Alterna tela cheia (modo console) |
| F3 | Mostra/esconde o contador de quadros (fps, tempo do quadro e o pior quadro do último segundo) |

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
- O objeto de `main.doo` é a **raiz**: é criado quando o jogo abre. Se a raiz se destruir (`destroy_self()`),
  o jogo fecha e o console volta ao menu.
- Todos os `.doo` da pasta são compilados juntos, então os objetos se enxergam pelo nome (`spawn(Inimigo, pos)`).
- `info.txt`: a linha `titulo:` é o nome mostrado no menu (sem ela, aparece o id).

## 4. Primeiro jogo

`games/ola/main.doo`:

```doo
object Ola

var x = 300
var cor = 0xFFCC00

function update() {
    if (input.pressed(Button.Right)) { x += 200 * time.delta }
    if (input.pressed(Button.Left))  { x -= 200 * time.delta }
    if (input.just_pressed(Button.A)) {
        cor = rgb(math.random() * 255, 200, 80)
        audio.play(660, 80)
    }
}

function draw() {
    render.clear(0x101820)
    render.rect(x, 220, 40, 40, cor)
    render.text(20, 20, "Olá, Doodle!", 28, 0xFFFFFF)
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
| objeto | o que `spawn` devolve | Referência gerenciada (`ref<Object>`) a uma instância |

**Verdadeiro/falso:** `nil`, `false` e `0` são falsos; uma referência a objeto destruído também. Todo o resto é
verdadeiro (inclusive `""`, `[]` e `vec3()`).

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
| Atribuição | `= += -= *= /=` (é um comando, não uma expressão) |

`==` entre tipos diferentes é sempre falso (`1 == true` é falso). Não há `++`/`--`: use `i += 1`.

### 5.4 Controle de fluxo

```doo
if (vida <= 0) {
    destroy_self()
} else if (vida < 20) {
    audio.play(220, 100)
}

while (x < 10) { x += 1 }

for (var i = 0; i < len(itens); i += 1) {
    print(itens[i])
}
```

Não há `break`/`continue`; use `return` dentro de uma função ou uma condição no laço.

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
push(itens, "espada")
itens[0] = "escudo"
itens[1 - 1] += "!"
print(len(itens))     // 1
```

Índice fora do array é erro de execução.

### 5.7 vec3

```doo
var p = vec3(0, 1, 0)
p.y += 2                          // muda só a componente
var q = p + vec3(1, 0, 0) * 3     // vec3 + vec3, vec3 * número, número * vec3, vec3 / número, -vec3
```

### 5.8 Funções da linguagem

| Função | Descrição |
|---|---|
| `len(x)` | Tamanho de array ou string |
| `push(array, valor)` | Acrescenta no fim (muda o array) |
| `print(a, b, ...)` | Escreve no console do simulador |
| `type(x)` | `"Player"` para objetos; senão `"número"`, `"string"`, `"array"`, `"vec3"`, `"bool"`, `"nil"` |
| `is(obj, Tipo)` | Verdadeiro se `obj` é `Tipo` ou estende `Tipo` |
| `vec3(x, y, z)` / `vec3()` | Cria um vetor (o segundo é zero) |
| `rgb(r, g, b)` | Cor `0xRRGGBB` a partir de 0..255 |
| `destroy_self()` | Marca este objeto para destruição no fim do quadro |
| `math.sin/cos/sqrt/abs/floor(x)` | Matemática (ângulos em radianos) |
| `math.min(a, b)`, `math.max(a, b)` | |
| `math.random()` | Número aleatório de 0 (incluso) a 1 (excluso) |
| `math.pi` | Constante |

## 6. Objetos e ciclo de vida

### 6.1 Funções chamadas pelo console

| Função | Quando roda |
|---|---|
| `create()` | Uma vez, logo depois de o objeto ser criado (e depois de `spawn` pôr a posição) |
| `update()` | Todo quadro: lógica e controle |
| `draw()` | Todo quadro, depois da física: desenho |
| `on_collision(other)` | Todo quadro enquanto encosta em outro objeto (física) |
| `destroy()` | Uma vez, quando o objeto é destruído |

Ordem de cada quadro: `update` de todos → física (e `on_collision`) → `draw` de todos → `destroy` de quem morreu.
Os objetos rodam na ordem em que foram criados (a raiz primeiro). Todas as funções são opcionais.

Desenhe no `draw` e declare câmera e luzes no `update`, assim tudo o que é desenhado no quadro já usa a
câmera e as luzes daquele quadro. Desenhos 2D ficam sempre por cima do 3D.

### 6.2 Criar e conversar com objetos

```doo
var inimigo = spawn(Inimigo, vec3(3, 0, 5))   // cria já com position; create() roda aqui
inimigo.vida -= 10                            // lê e altera campos de outro objeto
inimigo.tomar_dano(10)                        // chama funções de outro objeto
if (inimigo) { print("ainda vivo") }          // referência fica falsa depois de destruído
if (type(other) == Player) { ... }            // o tipo exato
if (is(other, BasicCharacterController)) { ... }  // o tipo ou algum descendente
```

- `spawn(Objeto, posição)`: a posição só é aplicada se o objeto tiver um campo `position`.
- Alterar membro (`a.b = ...`) só funciona quando `a` é uma variável; `lista[i].vida = 0` não compila
  (use `var e = lista[i]` e depois `e.vida = 0`).
- Usar um objeto já destruído é erro de execução.
- Não existe `self`: um objeto não passa a si mesmo como argumento.

## 7. Herança

```doo
// Heroi.doo
object Heroi extends BasicCharacterController

var jump_sound = "pulo.wav"   // muda o valor inicial de um campo do pai
var moedas = 0                // campo novo

function update() {
    super.update()            // a versão do pai
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
| `BoxCollider` | `size` vec3(1, 1, 1), `trigger` false | Caixa alinhada aos eixos, centrada em `position` |
| `SphereCollider` | `radius` 0.5, `trigger` false | Esfera |
| `CapsuleCollider` | `radius` 0.5, `height` 2, `trigger` false | Cápsula em pé |
| `Rigidbody` | `velocity` vec3(), `gravity` 20, `grounded` false | A física move o objeto e o empurra para fora do que é sólido |
| `TerrainCollider` | `size` vec3(10, 1, 10), `heights` nil | Chão com relevo (veja o prefab `Terrain`) |
| `AudioSource` | `sound` "", `volume` 1, `loop` false, `range` 20 | Som no mundo (veja `audio.source_play`) |

Física:

- Só objetos com `Rigidbody` se movem; eles são empurrados para fora de colisores sólidos e do terreno.
  `grounded` fica verdadeiro quando estão apoiados em algo.
- `trigger = true`: não bloqueia, só gera `on_collision` (moedas, zonas).
- `on_collision(other)` é chamado nos dois lados de cada par que se toca e que tem ao menos um `Rigidbody`.
- Colisores não giram: caixas ficam alinhadas aos eixos e cápsulas ficam em pé.

## 9. API do SDK

Argumentos com `= valor` são opcionais. Cores são números `0xRRGGBB`; `alpha` vai de 0 a 1. Caminhos de
arquivo são relativos à pasta do jogo.

### 9.1 `render` — 2D

| Função | Descrição |
|---|---|
| `render.clear(cor)` | Pinta a tela inteira (e limpa a profundidade) |
| `render.rect(x, y, w, h, cor, alpha = 1)` | Retângulo |
| `render.gradient(x, y, w, h, cor_topo, cor_base, alpha_topo = 1, alpha_base = 1)` | Gradiente vertical |
| `render.text(x, y, texto, tamanho, cor, alpha = 1)` | Texto (`y` = topo) |
| `render.text_width(texto, tamanho)` | Largura em pixels (para centralizar) |
| `render.image(caminho, x, y, w, h, alpha = 1)` | PNG, JPG, GIF ou BMP; devolve `false` se o arquivo não existir. Rosa `0xFF00FF` vira transparente |

### 9.2 `render` — 3D

| Função | Descrição |
|---|---|
| `render.camera(posição, alvo, fov = 60)` | Câmera em perspectiva (vale para o quadro) |
| `render.mesh(malha, posição, rotação, escala, cor, textura = "")` | Desenha uma malha. `malha` é `Mesh.Cube/Sphere/Cylinder/Capsule/Plane` ou um arquivo `.obj`. `rotação` em graus (vec3, ordem da Unity). `escala` é número ou vec3. `cor` tinge (0xFFFFFF mantém as cores do modelo) |
| `render.light_directional(direção, cor, intensidade = 1)` | Luz tipo sol (direção para onde ela aponta) |
| `render.light_point(posição, cor, alcance, intensidade = 1)` | Lâmpada: some suavemente até `alcance` |
| `render.light_spot(posição, direção, cor, alcance, ângulo = 45, intensidade = 1)` | Holofote/lanterna; `ângulo` é a abertura do cone |
| `render.ambient(cor)` | Luz que chega em todo lugar |
| `render.set_shader(nome)` | Shader do 3D neste quadro (veja [Shaders](#11-shaders)) |
| `render.terrain(posição, tamanho, alturas, cor, textura = "", repetição = 1)` | Chão com relevo (use o prefab `Terrain`) |
| `render.heightmap(caminho)` | Lê uma imagem em tons de cinza: devolve linhas de alturas 0..1 |

- Primitivas em tamanho de Unity: cubo 1, esfera Ø1, cilindro e cápsula Ø1 × 2 de altura, plano 1 × 1.
- Modelos `.obj`: materiais do `.mtl` (`Kd` e `map_Kd`), polígonos de qualquer tamanho, normais calculadas se
  faltarem.
- Luzes e shader são declarados a cada quadro (no `update`). São até 8 luzes por quadro; sem nenhuma, a cena
  usa um sol padrão.
- Texturas: filtro "pixel duro" de perto, mipmaps de longe. A perspectiva é corrigida (sem o entortamento do
  PS1, a não ser com o shader `ps1`).

### 9.3 `input`

| Função | Descrição |
|---|---|
| `input.pressed(Button.X)` | Verdadeiro enquanto o botão está apertado |
| `input.just_pressed(Button.X)` | Verdadeiro só no quadro em que foi apertado |

Botões: `Button.Up/Down/Left/Right/A/B/X/Y/L/R/Start/Select/Home`.

### 9.4 `audio`

| Função | Descrição |
|---|---|
| `audio.play(hz, ms)` | Toca um tom (onda quadrada). Devolve um id |
| `audio.play("arquivo.wav", volume = 1)` | Toca um WAV (PCM 8/16 bits). Devolve um id |
| `audio.loop("arquivo.wav", volume = 1)` | Toca em loop até `audio.stop(id)` |
| `audio.stop(id)` / `audio.stop()` | Para um som / todos |
| `audio.source_play()` | Com `use AudioSource`: toca `sound` na posição do objeto; o volume cai com a distância até a câmera (zero em `range`) e acompanha `volume` ao vivo. Devolve um id |
| `audio.source_stop()` | Para os sons deste objeto |

Os sons se misturam (vários ao mesmo tempo). Todos param quando o jogo fecha; um som posicional também para
quando o objeto é destruído.

### 9.5 `time`

| Função | Descrição |
|---|---|
| `time.delta` | Segundos do último quadro, vezes a escala (0 com o jogo pausado) |
| `time.unscaled_delta` | Segundos reais do último quadro (para UI em pausa) |
| `time.scale` / `time.set_scale(s)` | Escala do tempo: 0 = pausado, 1 = normal, 0.5 = câmera lenta. A física segue a escala |
| `time.clock()` / `time.date()` | Hora `"14:05"` e data `"11/09"` do sistema |

### 9.6 `store`

| Função | Descrição |
|---|---|
| `store.save(chave, valor)` | Guarda número, texto ou bool no save deste jogo |
| `store.load(chave, padrão)` | Lê do save deste jogo (`padrão` se não houver) |
| `store.installed()` | Ids dos jogos instalados |
| `store.is_installed(id)` | |
| `store.title(id)` | Título do `info.txt` (ou o id) |
| `store.get_save_data(id)` | Save bruto de um jogo (`""` se não houver). Um jogo só lê o próprio |

A loja online ainda não existe: "instalado" significa ter uma pasta em `games/`.

### 9.7 `physics`

| Função | Descrição |
|---|---|
| `physics.terrain_height(x, z)` | Altura do terreno do próprio objeto (que usa `TerrainCollider`) em (x, z), ou `nil` fora dele. O prefab `Terrain` expõe isso como `height_at` |

### 9.8 `system` (só o firmware)

`system.launch(id)`, `system.set_volume(0..1)`, `system.delete_save(id)`: veja a seção 13. Um jogo que chame
`system.*` não compila.

## 10. Prefabs do SDK

Objetos prontos, escritos em Doo, em `sdk/prefabs/`. Todo programa os recebe; use com `spawn` ou estenda com
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

Funções: `forward()` (vec3 para onde olha), `control()`, `camera()`.

### ParticleSystem

```doo
var fx = spawn(ParticleSystem, posição)
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
var lampada = spawn(Light, vec3(0, 3, 0))
lampada.color = 0xFFAA55
lampada.range = 10
```

Campos: `type` (`Light.Point` padrão, `Light.Spot`, `Light.Directional`), `enabled`, `color`, `intensity`,
`range`, `direction` (vec3(0, -1, 0)), `angle` (45).

### Terrain

```doo
var chao = spawn(Terrain, vec3(0, 0, 0))
chao.heightmap = "relevo.png"    // branco = alto; o topo da imagem fica do lado -z
chao.size = vec3(80, 6, 80)      // largura, altura máxima, profundidade
chao.texture = "grama.png"
chao.tiling = 16
chao.build()                     // depois de ajustar os campos
var y = chao.height_at(10, -5)   // para pôr objetos no chão
```

Sem `heightmap`, é um plano. Usa `TerrainCollider`: quem tem `Rigidbody` anda por cima.

### UI: Canvas, Text, Image, Button, Slider

```doo
var menu = spawn(Canvas)
var fundo = menu.image("", 0, 0, 640, 480)   // path "" = painel da cor `color`
fundo.color = 0x000000
fundo.alpha = 0.6
var jogar = menu.button("Jogar", 220, 200, 200, 44)
var volume = menu.slider("Volume", 220, 260, 200, 0, 100, 80)
menu.text("Menu", 280, 120, 32)

// no update:
if (jogar.clicked) { ... }
if (volume.changed) { ... volume.value ... }
```

`Canvas`:

- Desenha os elementos na ordem em que foram criados, por cima do 3D.
- Cuida do foco: o direcional leva ao elemento mais próximo naquela direção, A aperta botões e
  esquerda/direita ajustam sliders.
- Campos: `visible`, `active` (false = só mostra, bom para HUD), `sounds`, `accent` (cor do foco).
- Funções: `text`, `image`, `button`, `slider`, `add(elemento)` e `focus_on(elemento)`.

| Elemento | Campos principais |
|---|---|
| todos (`UIElement`) | `x`, `y`, `w`, `h`, `visible`, `alpha`, `color`, `accent` |
| `Text` | `text`, `size`, `align` (`"left"` / `"center"`) |
| `Image` | `path` |
| `Button` | `text`, `size`, `background`, `clicked` (vale um quadro) |
| `Slider` | `label`, `value`, `min`, `max`, `step`, `changed` (vale um quadro) |

Elementos próprios: `object MeuWidget extends UIElement`, sobrescrevendo `paint(focused, t)` e, se interagir,
`focusable()`, `activate()`, `adjust(d)` e `reset()`.

Menu de pausa típico: `time.set_scale(0)`, `jogador.controllable = false`, `menu.visible = true`.

## 11. Shaders

`render.set_shader(nome)` escolhe o shader do 3D no quadro:

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

Sem suporte a shaders no driver, o console usa luz por vértice e `render.set_shader` devolve `false`.

## 12. Saves

```doo
function create() {
    recorde = store.load("recorde", 0)
}

function fim_de_partida(pontos) {
    if (pontos > recorde) {
        recorde = pontos
        store.save("recorde", recorde)
    }
}
```

Cada jogo tem um arquivo `saves/<id>.sav` (texto, uma chave por linha). O firmware mostra "Dados salvos" no
menu e pode apagar o save pelo menu de opções (Y).

## 13. Firmware e APIs de sistema

O firmware é um programa Doo com privilégios (`firmware/main.doo` + `firmware/Xmb.doo`): mostra o boot e o
menu estilo XMB, com as categorias Configurações, Jogos e Loja. Só ele pode usar:

| Função | Descrição |
|---|---|
| `system.launch(id)` | Abre um jogo (o firmware fica suspenso e volta quando o jogo sai) |
| `system.set_volume(v)` | Volume geral do console, 0..1 |
| `system.delete_save(id)` | Apaga o save de um jogo |

As configurações do firmware (volume e tema) ficam em `saves/sistema.sav`.

## 14. Erros e depuração

- **Erro de compilação ou de execução:** o console mostra uma tela vermelha com `arquivo:linha: mensagem`.
  HOME volta ao menu (se o erro foi no firmware, ele recarrega do disco).
- `doodle.exe --check`: compila tudo sem abrir janela e lista os erros.
- `print(...)` escreve na janela de console do simulador.
- Erros comuns:

| Mensagem | Causa |
|---|---|
| `'x' não foi declarada` | Faltou `var x` (campo ou local) |
| `f() recebe N argumento(s), veio M` | Número de argumentos errado |
| `só dá pra alterar membro de uma variável` | `lista[i].x = ...`: use uma variável intermediária |
| `'system.launch' é exclusiva do firmware` | Jogos não usam `system.*` |
| `... em um objeto que já foi destruído` | Referência a um objeto que não existe mais: teste `if (ref)` antes |
| `recursão profunda demais (stack overflow)` | Mais de 200 chamadas aninhadas |

## 15. Limites conhecidos

| Área | Limite atual |
|---|---|
| Linguagem | Sem `break`/`continue`, `self`, `++`, operador ternário, funções de string (dividir, buscar), dicionários |
| Números | Todos são ponto flutuante (sem int separado) |
| Física | Colisores sem rotação; dois `Rigidbody` só avisam o contato (não se empurram); qualquer rampa de terreno é caminhável |
| Render | Até 8 luzes por quadro; sem sombras; câmera só em perspectiva; modelos sem animação; terreno com uma textura só |
| Áudio | Só WAV PCM; som posicional só no volume (sem esquerda/direita) |
| UI | Só controle (sem mouse/toque); sem campo de texto nem listas com rolagem |
| Loja | Ainda não existe (seção 9 do documento de projeto) |
