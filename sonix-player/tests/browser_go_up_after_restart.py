#!/usr/bin/env python3
"""Regressao do bug relatado em PR #281: apos reiniciar, voltar da pasta filha
para a pasta pai deve restaurar o scroll do pai, nao voltar ao topo.

Sessao 1: scroll do pai ate 500, entrar na filha como um toque faria.
Sessao 2: reiniciar (o estado abre a filha) e voltar uma pasta.

As sessoes 3 e 4 repetem o caso com a listagem da filha atrasada: o reinicio
acontece antes de ela ser adotada, quando so o estado escrito na navegacao
existe. A pilha gravada nesse instante deve ser a da pasta que se sai.
"""

import argparse
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MAGIC = b"SNXBRW2"
FOLDER_OFFSET = 64
EXPECTED_PARENT_SCROLL = 500


def read_state(path):
    data = Path(path).read_bytes()
    if data[:8] != MAGIC + b"\x00" or len(data) < FOLDER_OFFSET:
        raise ValueError("bad state magic: %r" % data[:8])
    scroll = struct.unpack_from("<i", data, 8)[0]
    stack_len = struct.unpack_from("<i", data, 12)[0]
    stack = list(struct.unpack_from("<12i", data, 16))
    folder = data[FOLDER_OFFSET:].split(b"\x00", 1)[0].decode()
    return folder, scroll, stack_len, stack[:stack_len]


def run_host(host, env, log):
    return subprocess.Popen(
        [host],
        env={**os.environ, **env},
        stdout=subprocess.DEVNULL,
        stderr=open(log, "w"),
    )


def wait_for(path, predicate, log, timeout=30):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            last = read_state(path)
            if predicate(last):
                return last
        except (FileNotFoundError, ValueError):
            pass
        time.sleep(0.2)
    with open(log) as f:
        tail = f.read()[-2000:]
    raise TimeoutError("state never matched; last=%r\n--- simulator log ---\n%s" % (last, tail))


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="./sonix_player_host")
    args = ap.parse_args()
    host = str(Path(args.host).resolve())
    if not Path(host).is_file():
        print("host binary not found: " + host, file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="sonix-goup-") as tmp:
        root = Path(tmp)
        card = root / "card"
        music = card / "Music"
        artist = music / "Artist"
        album = artist / "Album"
        album.mkdir(parents=True)
        # Music tem de ter linhas suficientes para que um scroll de 500 seja
        # possivel: e assim que se distingue "topo" de "scroll do Artista".
        for i in range(40):
            (music / ("track-%02d.mp3" % i)).write_bytes(b"")
        for i in range(40):
            (artist / ("track-%02d.mp3" % i)).write_bytes(b"")
        for i in range(3):
            (album / ("song-%02d.mp3" % i)).write_bytes(b"")

        cfg = str(root / "device_config.ini")
        state = cfg + ".browser"
        base = dict(
            SONIX_SD_ROOT=str(card),
            SONIX_CONFIG=cfg,
            SONIX_OPEN_BROWSER="1",
            SONIX_NO_MOUNT="1",
            SONIX_NO_SYSSERVER="1",
            SDL_VIDEODRIVER="dummy",
            SDL_AUDIODRIVER="dummy",
        )

        # --- Sessao 1: scroll do pai, entrar na filha ---
        p1 = run_host(host, {
            **base,
            "SONIX_BROWSER_OPEN_DIR": str(artist),
            "SONIX_BROWSER_SCROLL": str(EXPECTED_PARENT_SCROLL),
            "SONIX_BROWSER_OPEN_CHILD": str(album),
        }, str(root / "session1.log"))
        try:
            folder, scroll, stack_len, stack = wait_for(
                state, lambda v: v[0] == str(album), str(root / "session1.log")
            )
            print("sessao 1: pasta=%s scroll=%d pilha=%s" % (folder, scroll, stack))
            if EXPECTED_PARENT_SCROLL not in stack:
                print("FALHA: o scroll do pai nao foi persistido na pilha: %s" % stack, file=sys.stderr)
                return 1
        finally:
            stop(p1)

        # A reinicializacao mantem o ficheiro de estado: e exactamente o que
        # queremos testar. Um novo processo le o estado que a sessao 1 gravou.

        # --- Sessao 2: reiniciar e voltar uma pasta ---
        p2 = run_host(host, {
            **base,
            "SONIX_BROWSER_GO_UP_MS": "1500",
        }, str(root / "session2.log"))
        try:
            folder, scroll, stack_len, stack = wait_for(
                state, lambda v: v[0] == str(artist), str(root / "session2.log")
            )
        finally:
            stop(p2)

        print("sessao 2: pasta=%s scroll=%d pilha=%s" % (folder, scroll, stack))
        if scroll != EXPECTED_PARENT_SCROLL:
            print("FALHA: esperado scroll do pai=%d, obtido %d" % (EXPECTED_PARENT_SCROLL, scroll), file=sys.stderr)
            return 1

        # --- Sessao 3: entrar na filha, mas reiniciar antes da listagem ---
        # A leitura da filha e atrasada de proposito. O estado deixado no
        # cartao e entao o que a navegacao escreveu, e nao o que adopt_listing()
        # sobrescreveria depois: e esse que um desligamento abrupto apanha.
        Path(state).unlink(missing_ok=True)
        p3 = run_host(host, {
            **base,
            "SONIX_BROWSER_OPEN_DIR": str(artist),
            "SONIX_BROWSER_SCROLL": str(EXPECTED_PARENT_SCROLL),
            "SONIX_BROWSER_OPEN_CHILD": str(album),
            "SONIX_BROWSER_DELAY_PATH": str(album),
            "SONIX_BROWSER_READ_DELAY_MS": "6000",
        }, str(root / "session3.log"))
        try:
            folder, scroll, stack_len, stack = wait_for(
                state, lambda v: v[0] == str(artist) and v[1] == EXPECTED_PARENT_SCROLL,
                str(root / "session3.log")
            )
        finally:
            stop(p3)

        print("sessao 3: pasta=%s scroll=%d pilha=%s" % (folder, scroll, stack))
        if stack_len != 0:
            print("FALHA: o scroll do Artista entrou na sua propria pilha: %s" % stack, file=sys.stderr)
            return 1

        # --- Sessao 4: reboot desse estado e voltar uma pasta ---
        # A pilha gravada na sessao 3 nao tem antepassados: "para cima" abre o
        # topo de Music. Com a ordem antiga o scroll 500 era gravado na pilha e
        # Music abria a essa altura.
        p4 = run_host(host, {
            **base,
            "SONIX_BROWSER_GO_UP_MS": "1500",
        }, str(root / "session4.log"))
        try:
            folder, scroll, stack_len, stack = wait_for(
                state, lambda v: v[0] == str(music), str(root / "session4.log")
            )
        finally:
            stop(p4)

        print("sessao 4: pasta=%s scroll=%d pilha=%s" % (folder, scroll, stack))
        if scroll != 0:
            print("FALHA: esperado topo de Music, obtido scroll=%d" % scroll, file=sys.stderr)
            return 1

    print("browser_go_up_after_restart: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
