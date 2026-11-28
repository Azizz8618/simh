# SIMH-стенд для регрессии NanoLang → P2C

Рабочая директория проверки десяти эталонов nano2p2c в полной ОС ДИСПАК.

## Состав (рабочие файлы)

- `drive_b_stock.py` — проверенный однозадачный драйвер; загрузка SIMH,
  `ДАЙ А0/А1/НОМВ`, колода через `at vu0`, завершение по `prn0`;
  с ним получено `10 passed, 0 failed` через `run-simh-tests.sh`.
- `setup_b_stock.py` — генератор ini: md41 = образ задания, md45 = стоковый
  том 2348 (рабочий 2348 с изменённым PERSO мешает загрузке), md07 = рабочая
  копия 2053.
- `p2c_only_perso.b6` — колода только с `*PERSO:413600`; `PASHELP`, `PASCOMPL`,
  `CCOM` и libc берутся из PERSO-библиотеки зон `3600..3627₈` тома 2348.

## Локальные артефакты (не включаются в Git)

`md41_single` (из `pas2c-metamorph/.localdocs/simh-p2c/`), `allperso.bin` (дамп
28 зон PERSO), `utxt2bin.py` (из `pas2c-metamorph/dispak/`), а также
порождаемые `img_*`, `decks/`, `runs/`, `images/`.

## Проверка

```sh
cd /home/azizz/Yandex.Disk/aesc-crypto-projects/tools/nano2p2c
sh run-simh-tests.sh       # 10 passed, 0 failed
```

Эмулятор закреплён по SHA `d058305796bcd0aa`, путь —
`/home/azizz/Yandex.Disk/simh/BESM6/BIN/besm6`, tmux изолирован через
`TMUX_TMPDIR=$PWD/runs/tmux`, серии не пересекаются через `flock simh.lock`.

## Опыт оптимизации (не применяется)

Пробовались `build_batch.py`/`drive_batch.py` (один SIMH-сеанс на все колоды)
и `drive_fast.py` (event-driven ожидания вместо фиксированных пауз). Оба дали
нестабильный результат: `PASHELP` корректно читает исходник только из зоны
`0023₈`, а повторное использование `prn0` в одной сессии теряет выдачу.
Поэтому сохранён проверенный однозадачный путь. Регулируемые счётчики
`send after=…` / `step …` из `BESM6/UVU/expect.ini` остаются образцом для
будущей оптимизации.
