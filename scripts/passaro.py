"""Gera os quadros-chave de um passaro: mesma malha, asas em alturas diferentes.

Cada arquivo tem os mesmos vertices na mesma ordem, que e o que o draw_mesh_mix exige.
"""
import io
import math

DEST = r"C:\Users\vinic\Doodle\games\test-3d"


def tris_corpo():
    f, tr = (0, 0, 0.55), (0, 0, -0.5)  # frente e cauda
    e, d = (-0.11, 0, 0), (0.11, 0, 0)  # esquerda e direita
    c, b = (0, 0.11, 0), (0, -0.09, 0)  # cima e baixo
    return [(f, d, c), (f, c, e), (f, e, b), (f, b, d),
            (tr, c, d), (tr, e, c), (tr, b, e), (tr, d, b)]


def tris_asa(lado, altura):
    x = 0.08 * lado
    ponta = 0.8 * lado
    raiz1, raiz2 = (x, 0.02, 0.14), (x, 0.02, -0.14)
    pt1, pt2 = (ponta, altura, -0.2), (ponta, altura, 0.08)
    if lado < 0:  # mantem as faces na mesma ordem dos dois lados
        return [(raiz1, raiz2, pt1), (raiz1, pt1, pt2)]
    return [(raiz2, raiz1, pt2), (raiz2, pt2, pt1)]


def normal(t):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    n = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
    return nx / n, ny / n, nz / n


for quadro, graus in enumerate([38, 0, -30], start=1):
    altura = math.sin(math.radians(graus)) * 0.62
    tris = tris_corpo() + tris_asa(-1, altura) + tris_asa(1, altura)
    linhas = ["# passaro, quadro %d (asa em %d graus) - gerado por scripts/passaro.py" % (quadro, graus)]
    for t in tris:
        for v in t:
            linhas.append("v %.4f %.4f %.4f" % v)
    for t in tris:
        linhas.append("vn %.4f %.4f %.4f" % normal(t))
    for i in range(len(tris)):
        a, b, c = i * 3 + 1, i * 3 + 2, i * 3 + 3
        linhas.append("f %d//%d %d//%d %d//%d" % (a, i + 1, b, i + 1, c, i + 1))
    io.open("%s\\passaro%d.obj" % (DEST, quadro), "w", encoding="utf-8", newline="\n").write("\n".join(linhas) + "\n")
    print("passaro%d.obj: %d triangulos" % (quadro, len(tris)))
