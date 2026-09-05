#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Имя инструмента не попадает в приложение и его справку.

Распоряжение автора, действующее во всех репозиториях профиля (`CLAUDE.md`,
раздел «Авторство: инструмент его не имеет»):

    Материал, задача и результат принадлежат автору проекта
    (dtba3a <dtba3a@gmail.com>). Работа ведётся нанятым инструментом.
    Инструмент авторства не имеет и на него не претендует.

Повторное нарушение 2026-09-02: в справке приложения стоял заголовок «Правила
ведения мануала (из CLAUDE.md)» — правило автора было приписано инструменту, а
имени автора рядом не было вовсе. Замечание автора: «это требование уже было…
в приложении нет имени автора против CLAUDE везде».

Правило, которое надо вспомнить, не работает; работает то, что срабатывает
само. Поэтому проверка стоит в сборке: она смотрит ровно то, что доходит до
оператора, — исходники приложения, его ресурсы и его справку.

Что НЕ проверяется и почему: инженерные записки в `docs/` вне `docs/manual/`,
`references/`, `chatlog/`, `sessions/` и сам `CLAUDE.md`. Там имя инструмента
есть факт о среде и о том, где лежит файл, а не заявление об авторстве;
вычищать факты запрещено отдельно («опровергнутое не удаляется»).

Запуск:  python3 tools/check_app_authorship.py
Выход:   0 — чисто, 1 — найдено (сборку в CI останавливает).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# То, что доходит до оператора: сам код, ресурсы, справка приложения.
AREAS = ["ctpu/src", "ctpu/res", "docs/manual"]

# Имя инструмента в любом виде, включая склейки вроде «CLAUDE.md».
TOOL_NAME = re.compile(r"claude|anthropic", re.IGNORECASE)

# Автор обязан быть назван в окне «О программе» и в справке.
AUTHOR_REQUIRED = {
    "ctpu/src/mainwindow.cpp": "dtba3a",
    "docs/manual/README.md": "dtba3a",
}


def scan():
    hits = []
    for area in AREAS:
        base = ROOT / area
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue  # двоичные ресурсы: значки, шрифты
            for n, line in enumerate(text.splitlines(), 1):
                if TOOL_NAME.search(line):
                    hits.append((path.relative_to(ROOT).as_posix(), n, line.strip()[:100]))
    return hits


def missing_author():
    absent = []
    for rel, name in AUTHOR_REQUIRED.items():
        path = ROOT / rel
        if not path.exists():
            absent.append((rel, "файла нет"))
        elif name not in path.read_text(encoding="utf-8", errors="replace"):
            absent.append((rel, "имя автора «%s» не названо" % name))
    return absent


def main():
    print("=== АВТОРСТВО: имя инструмента в приложении и справке ===")
    hits = scan()
    absent = missing_author()

    if hits:
        print("\nИмя инструмента в том, что видит оператор:")
        for rel, n, line in hits:
            print("   %s:%d  %s" % (rel, n, line))
    else:
        print("\nИмени инструмента в приложении и справке нет.")

    if absent:
        print("\nИмя автора не названо там, где обязано быть:")
        for rel, why in absent:
            print("   %s — %s" % (rel, why))
    else:
        print("Имя автора названо везде, где обязано.")

    total = len(hits) + len(absent)
    print("\nИтог: нарушений %d." % total)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
