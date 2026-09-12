# Monta a pasta que o servidor da loja serve (store-backend/loja) a partir de games/.
#
#   .\store-backend\publicar.ps1              publica todos os jogos
#   .\store-backend\publicar.ps1 quadrado     publica só um
#
# Depois é só servir a pasta como arquivo estático:
#   python -m http.server 8080 --directory store-backend\loja
param([string[]]$jogos)

$raiz = Split-Path -Parent $PSScriptRoot
$origem = Join-Path $raiz "games"
$destino = Join-Path $PSScriptRoot "loja"

if (-not $jogos) {
    $jogos = (Get-ChildItem $origem -Directory | Where-Object { Test-Path (Join-Path $_.FullName "main.doo") }).Name
}
if (Test-Path $destino) { Remove-Item $destino -Recurse -Force }
New-Item -ItemType Directory $destino | Out-Null

$linhas = New-Object System.Collections.Generic.List[string]
foreach ($id in $jogos) {
    $pasta = Join-Path $origem $id
    if (-not (Test-Path (Join-Path $pasta "main.doo"))) {
        Write-Warning "sem main.doo, pulando: $id"
        continue
    }
    $alvo = Join-Path $destino $id
    Copy-Item $pasta $alvo -Recurse
    $marca = Join-Path $alvo ".loja"
    if (Test-Path $marca) { Remove-Item $marca }   # marca de instalação, não se republica

    $titulo = $id
    $info = ""
    $infoPath = Join-Path $pasta "info.txt"
    if (Test-Path $infoPath) {
        foreach ($l in Get-Content $infoPath -Encoding UTF8) {
            if ($l -match '^\s*titulo:\s*(.+)$') { $titulo = $Matches[1].Trim() }
            if ($l -match '^\s*descricao:\s*(.+)$') { $info = $Matches[1].Trim() }
        }
    }
    $linhas.Add("jogo`t$id")
    $linhas.Add("titulo`t$titulo")
    if ($info -ne "") { $linhas.Add("info`t$info") }
    foreach ($a in Get-ChildItem $alvo -Recurse -File) {
        $rel = $a.FullName.Substring($alvo.Length + 1).Replace('\', '/')
        $linhas.Add("arquivo`t$rel`t$($a.Length)")
    }
    $linhas.Add("")
    "publicado: $id ($titulo)"
}

# sem BOM: o console lê o catálogo como texto puro
[IO.File]::WriteAllLines((Join-Path $destino "catalogo.txt"), $linhas, (New-Object System.Text.UTF8Encoding $false))
"catalogo.txt com $($jogos.Count) jogo(s) em $destino"
