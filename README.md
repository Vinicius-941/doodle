# Doodle

Console experimental com linguagem própria (**Doo**), compilador, máquina virtual,
simulador para Windows, firmware e SDK para desenvolvimento de jogos.
O compilador, a VM e o simulador são implementados em C++; o firmware, os prefabs
do SDK e os jogos são escritos em Doo.

## Vibe Coding

Este projeto foi desenvolvido com **Vibe Coding**, usando inteligência artificial
para auxiliar na criação e evolução do código a partir de instruções em linguagem natural.

## Compilar e executar

Requisitos: Windows, Visual Studio com as ferramentas de desenvolvimento C++
e CMake 3.20 ou superior.

```powershell
cmake -B build
cmake --build build --config Release
.\build\Release\doodle.exe
```

Use `--console` para abrir direto em tela cheia (F11 ou Alt+Enter alterna a
qualquer momento) e `--fps` para ligar o contador de quadros, que F3 também
mostra e esconde.

O simulador abre o firmware, que permite selecionar os jogos disponíveis.
Os arquivos `.doo` são compilados ao carregar o programa; editar um jogo não
exige recompilar o simulador. Mantenha as pastas de código e recursos no local
usado na configuração do CMake.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `doo-compiler/` | Compilador da linguagem Doo |
| `vm-runtime/` | Máquina virtual e execução do bytecode |
| `simulator/` | Simulador Windows: renderização OpenGL, input e áudio |
| `firmware/` | Boot e menu do console, escritos em Doo |
| `sdk/` | Física, modelos, áudio, saves e prefabs em Doo |
| `games/` | Jogos: Quadrado, teste 3D e DooCraft |
| `store-backend/` | Loja digital: servidor estático e script de publicação |
| `tests/` | Testes do compilador, runtime e SDK |
| `docs/` | Manual da linguagem e do SDK |

## Loja

A loja é um servidor de arquivos estáticos — sem framework, sem banco, sem login. Para pôr no ar:

```powershell
.\store-backend\publicar.ps1
python -m http.server 8080 --directory store-backend\loja
```

No console, a terceira coluna do menu lista o catálogo: **A** instala (ou abre, se já estiver instalado) e
**X** atualiza a lista. Detalhes do formato em [store-backend/README.md](store-backend/README.md).

## Controles

Setas controlam o direcional; **Z / X / A / S** correspondem aos botões
**A / B / X / Y**. **Q / W** são L / R, **Enter** é Start,
**Backspace** é Select e **Esc** retorna ao firmware (HOME).
Também há suporte a controle XInput.

Consulte os controles específicos do [DooCraft](games/doo-craft/README.md).

## Verificação

```powershell
.\build\Release\doodle.exe --check
ctest --test-dir build -C Release --output-on-failure
```

O primeiro comando compila o firmware e todos os jogos sem abrir janela.
O segundo executa os testes automatizados. Builds e saves locais ficam fora
do controle de versão.

Veja o [manual da linguagem Doo e do SDK](docs/manual.md) para as APIs,
exemplos de uso e limitações atuais.
