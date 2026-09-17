# Monta o console para entregar: uma pasta (e um zip) com tudo o que ele precisa para rodar em qualquer
# Windows, sem instalar nada.
#
#   .\scripts\empacotar.ps1              jogos compilados (sem o codigo-fonte)
#   .\scripts\empacotar.ps1 -Fonte       jogos com os .doo, para quem for mexer neles
#   .\scripts\empacotar.ps1 -SemZip      so a pasta
#
# Sai em dist\Doodle\ e dist\Doodle.zip.
param([switch]$Fonte, [switch]$SemZip)

$ErrorActionPreference = "Stop"
$raiz = Split-Path -Parent $PSScriptRoot
$doodle = Join-Path $raiz "build\Release\doodle.exe"
$destino = Join-Path $raiz "dist\Doodle"

if (-not (Test-Path $doodle)) { throw "compile antes: cmake --build build --config Release (falta $doodle)" }

if (Test-Path $destino) { Remove-Item $destino -Recurse -Force }
New-Item -ItemType Directory $destino -Force | Out-Null

Copy-Item $doodle $destino
Copy-Item (Join-Path $raiz "firmware") (Join-Path $destino "firmware") -Recurse
New-Item -ItemType Directory (Join-Path $destino "sdk") -Force | Out-Null
Copy-Item (Join-Path $raiz "sdk\prefabs") (Join-Path $destino "sdk\prefabs") -Recurse
New-Item -ItemType Directory (Join-Path $destino "games") -Force | Out-Null
New-Item -ItemType Directory (Join-Path $destino "saves") -Force | Out-Null

foreach ($pasta in Get-ChildItem (Join-Path $raiz "games") -Directory) {
    $id = $pasta.Name
    if (-not (Test-Path (Join-Path $pasta.FullName "main.doo"))) { continue }
    $alvo = Join-Path $destino "games\$id"
    Copy-Item $pasta.FullName $alvo -Recurse
    Get-ChildItem $alvo -Recurse -File | Where-Object { $_.Extension -eq '.loja' } | Remove-Item
    if (-not $Fonte) {
        $saida = & $doodle --build "games/$id" (Join-Path $alvo "jogo.doobc")
        if ($LASTEXITCODE -ne 0) { throw "nao compilou $id`n$saida" }
        # extensao exata: o -Filter *.doo do Windows tambem pega jogo.doobc (pelo nome curto 8.3)
        Get-ChildItem $alvo -Recurse -File | Where-Object { $_.Extension -eq '.doo' } | Remove-Item
    }
    "incluido: $id"
}

$leiame = @"
Doodle - console caseiro

Para ligar, e so abrir doodle.exe. Nao precisa instalar nada.

  Direcional: setas          A / B / X / Y: Z / X / A / S
  L / R: Q / W               Gatilhos: E / R
  Start / Select: Enter / Backspace      HOME (sai do jogo): Esc
  Analogico esquerdo: setas  Analogico direito: I / J / K / L
  Controle de XInput (Xbox) funciona plugado, ate dois jogadores.

  F11 ou Alt+Enter: tela cheia       F3: contadores de desempenho

A lista de botoes tambem esta no console, em Configuracoes > Controles.
Para fechar o console, use "Desligar o console", na coluna Configuracoes.

A tela do console tem 320 x 180 pixels e roda a 60 quadros por segundo.
Os jogos ficam em games\, um por pasta; os saves, em saves\.
A loja procura o servidor gravado em saves\sistema.sav (padrao http://localhost:8080).
"@
$leiame | Out-File (Join-Path $destino "LEIAME.txt") -Encoding utf8

if (-not $SemZip) {
    $zip = Join-Path $raiz "dist\Doodle.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $destino -DestinationPath $zip
    "zip: $zip"
}
"pronto: $destino"
