# DooCraft

Jogo voxel em primeira pessoa, inspirado no modo criativo do Minecraft clássico.
Toda a lógica executável do jogo está em `main.doo`: geração, mundo, colisão,
movimento, mira DDA, edição, saves e HUD. Usa o Canvas do SDK para o menu.
Não requer alterações no firmware, compilador, VM ou SDK.

## Jogar

Abra `C:\Users\vinic\Doodle\build\Release\doodle.exe` e selecione
**DooCraft - Mundo de Blocos** na categoria Jogos. Se o console já estava aberto,
reinicie-o para atualizar a lista. O carregamento inicial é dividido em quadros.

| Teclado | Ação | Controle Doodle |
|---|---|---|
| Cima / baixo | Andar para frente / trás | Up / Down |
| Esquerda / direita | Girar câmera | Left / Right |
| Backspace + cima / baixo | Olhar para cima / baixo | Select + Up / Down |
| Backspace + esquerda / direita | Passo lateral | Select + Left / Right |
| Z | Pular | A |
| A | Quebrar bloco (segurar repete) | X |
| X | Colocar bloco (segurar repete) | B |
| S | Copiar material da mira | Y |
| Q / W | Material anterior / seguinte | L / R |
| Enter | Abrir / fechar pausa | Start |
| Esc | Voltar ao firmware | HOME |

O SDK atual não expõe mouse nem um Select no mapeamento XInput; use teclado
para olhar verticalmente e dar passos laterais. As letras do teclado e os nomes
dos botões do console são diferentes: **A no teclado quebra; X no teclado coloca**.

## Mundo e construção

- Mundo finito de **48 × 48 × 32**, com 73.728 células e alcance visual de 16 blocos.
- Ruído de valor interpolado suavemente, em quatro escalas, com semente 73129.
- Montanhas, grama, três camadas de terra, pedra abaixo, areia nas áreas baixas,
  árvores de troncos e folhas e uma camada inferior indestrutível.
- Sete materiais com estoque ilimitado: grama, terra, pedra, areia, madeira,
  folhas e tijolos. As texturas e o ícone são originais, gerados para este jogo.
- Mira até seis blocos; contorno marca o alvo. A colocação usa a célula adjacente
  à face atingida. Não permite colocar blocos dentro do jogador.
- Colisão por grade com subpassos, gravidade e pulo. As bordas e o teto limitam
  o mundo. Não há auto-step: pule para subir um bloco.
- Somente voxels expostos são enviados para desenho, com cache por coluna,
  descarte por distância e direção. Editar atualiza a coluna e seus vizinhos.

As edições são salvas imediatamente em `saves/doo-craft.sav`. O jogo regenera a
mesma semente e reaplica as edições ao entrar. Posição, câmera e material são
salvos a cada oito segundos e ao abrir a pausa. Use **Salvar e voltar ao console**
para salvar a posição exata antes de sair. Esc preserva todas as edições, mas a
posição pode voltar ao último salvamento periódico. O firmware pode apagar o
save para recomeçar. O histórico de edições cresce com o uso; não há compactação.

Este é um sandbox criativo funcional, não uma reprodução integral de uma versão
do Minecraft. Não inclui mundo infinito, crafting, sobrevivência, criaturas,
multiplayer, líquidos, iluminação por voxel ou ciclo dia/noite.

## Validação

- `build\Release\doodle.exe --check`: firmware e todos os jogos compilam.
- `ctest --test-dir build -C Release --output-on-failure`: 2/2 testes passam.
- Harness externo usando o compilador, VM e SDK reais: geração completa, relevo,
  estratos, árvores, spawn seguro, gravidade, pulo e pouso, rocha indestrutível,
  rejeição de construção dentro do jogador, mira DDA e adjacência, quebra,
  reposição e restauração exata do mundo via replay de saves em memória.
- Renderização real OpenGL em janela oculta, sem erros de GL ou de execução;
  aproximadamente 5,2 ms por quadro em 60 quadros no cenário testado nesta máquina.
  Isso não é garantia de desempenho no hardware de referência ou em todo cenário.
