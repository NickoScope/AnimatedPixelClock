# tools/twin — the virtual panel

The real firmware image, unchanged, on an emulated ESP32-S3 and a model of the 128x64 HUB75 panel.
You see on the Mac what the LEDs would show. The remote and the knob are pin-level models, and the
firmware decodes them itself. Why it is built this way, and what it can never match: knowledge
base `docs/40-virtual-twin.md` (ADR-TWIN-01).

## What runs

- **Engine:** [esp32sim](https://github.com/joakimeriksson/esp32sim) (Rust, MIT) at upstream
  `4ab7e90`, plus this project's changes in `engine/esp32sim-twin.patch`. The patch is one diff
  against upstream, checked to reproduce our branch exactly; `engine/HISTORY.txt` lists the
  commits behind it.
- **The patches add:**
  - the octal flash our module has (a Macronix ID, `--flash-id`), and 4-byte and octal command bytes;
  - LCD_CAM in i8080 mode, streaming its GDMA ring word by word to the board at the programmed PCLK;
  - an SD/MMC host with an empty slot (SD_MMC fails as it does with no card);
  - the radio calibration completing without an AP;
  - `--board hub75-panel` (alias `panel`): the `hub75` crate decodes the bus words into each LED's
    light, one GDMA descriptor at a time, a refresh per SUC_EOF;
  - the panel's inputs as pin-level devices: a TSOP on GPIO0 sending the owner's NEC codes (with
    repeats while held), the EC11 knob on IO45/IO46, BOOT; GPIO0 is their wired-AND;
  - `--hostfwd tcp|udp:HOST-GUEST`: the twin's portal, API and UDP port on 127.0.0.1;
  - a chip reset keeps the virtual AP and network (the world outside the chip);
  - `--cpi N`: uniform cycles per instruction (a timing knob; see calibrate.py);
  - `--flash-persist` (the flash chip is a file, written through) and `--serial-hex`;
  - `web/panel.html`: the panel as LED light, the remote, the knob and the console.

## Setup (once)

```
brew install rustup && rustup default stable
git clone https://github.com/joakimeriksson/esp32sim ~/twin/esp32sim
cd ~/twin/esp32sim && git checkout -b nickoscope/twin 4ab7e90 && git apply <this repo>/tools/twin/engine/esp32sim-twin.patch
cargo build --release
```

- **The ESP32-S3 mask ROM** comes from Espressif's esp-rom-elfs releases: `esp32s3_rev0_rom.elf` into `~/twin/rom/`.
- **The firmware:** a full image goes to `~/twin/fw/<version>/merged.bin`, with its `firmware.elf` beside it.
- **eFuse:** `~/twin/efuse-opi.txt` holds one line, `0x6000703c: 00000200`. That is FLASH_TYPE = 1 (octal flash), as fused in the WROOM-2 module, from `EFUSE_RD_REPEAT_DATA3_REG` bit 9 in `soc/esp32s3/register/soc/efuse_reg.h`.

## Use

```
tools/twin/twin.py wifi NickoTwin twin-demo-2026     # the virtual AP, and what Improv hands over ("" = open; no ',')
tools/twin/twin.py run --fresh --provision --web 8790 # a new chip; Improv at boot; http://127.0.0.1:8790/panel.html
tools/twin/twin.py run --web 8790                     # later runs: the chip remembers
tools/twin/twin.py run --seconds 30 --png shot.png    # headless, as fast as the Mac allows
tools/twin/twin.py flash firmware.bin                 # an app image at 0x10000, otadata back on app0, like pio upload
tools/twin/twin.py run --seconds 50 -- --script tools/twin/learn_remote.txt   # teach it the remote
```

Verified with the real v2.7.3 image, unmodified (2026-09-29):
- **Boot:** ROM → bootloader → app, octal flash, 16 MB PSRAM.
- **Display:** the setup screen, then the clock with NTP time, drawn from the HUB75 stream at the 10 MHz PCLK. There are 106.1 refreshes a second, as derived.
- **Wi-Fi:** provisioned over Improv-Serial; the twin reboots and joins the virtual AP.
- **Portal and API:** at `127.0.0.1:8080`.
- **Lua:** OCEANARIUM uploaded over `/api/lua/upload`; it runs at 15.2 fps.
- **Remote:** the ten remote codes are learned by the firmware's own `ir learn`, and the ▶ key changes the clock style.
- **Knob:** turning it does the same, counted in `/api/knob`.
- **OTA:** the release OTA image went through `/update` (tools/agent/update.py). The twin rebooted into app1, and its health check confirmed the image after 61 s and 200 frames.

## Веб-прошивальщик

Двойника прошивает та же страница, что и панель: ESP Web Tools 10.4.0 с esptool-js 0.6.0 внутри.
Сброс в режим загрузки по DTR/RTS, stub, стирание, запись, сброс в прошивку и Improv Wi-Fi идут
через настоящий ROM двойника и его USB-Serial/JTAG.

- **Как устроено.** `twin.py run --web PORT` при каждом запуске собирает `state/web/`: страницы
  движка и `flasher/`. `flasher/` — копия `docs/` (index.html, flasher.js, styles.css, img/,
  образ из `docs/firmware/latest`). В `index.html` четыре правки, каждая по якорю, который обязан
  найтись ровно один раз:
  - первым в `<head>` подключается шим `flasher/twin-serial.js`;
  - ESP Web Tools закреплён на 10.4.0 (в `docs/` стоит `@10`);
  - к заголовку добавлено «Двойник · »;
  - сверху строка о том, чья это страница.

  Сама `docs/` не меняется.
- **Шим.** Это `navigator.serial` с одним портом 303A:1001 поверх `ws://…/usj`, протокол движка.
  Открытие и закрытие порта линии DTR/RTS не трогают. Каждый `setSignals` передаёт пару линий,
  DTR раньше RTS. По кнопке Install появляется выбор:
  - «Двойник»;
  - «Плата по USB…» — родной выбор порта в Chrome, так что настоящую плату с этой страницы тоже можно прошить;
  - «Отмена».
- **Движок нужен с `/usj`** (ветка `nickoscope/usj`, в `engine/esp32sim-twin.patch` её ещё нет).
  До слияния запускайте с `TWIN_ENGINE=~/twin/wt-usj`. Со старым движком страница откроется,
  но порт — нет: `/usj` там отвечает 404, и ESP Web Tools покажет «Failed to open serial port».

```
tools/twin/twin.py wifi NickoTwin twin-demo-2026   # виртуальная точка: её предложит Improv
tools/twin/twin.py run --blank --web 8790          # новый чип, пустой (ROM: invalid header)
#   http://127.0.0.1:8790/flasher/index.html → Connect → «Двойник» → Install AnimatedPixelClock
#   → Erase device → Install; затем Configure Wi-Fi: NickoTwin и пароль из `twin.py wifi`
tools/twin/twin.py verify                          # flash двойника = образ вне data-разделов?
python3 tools/twin/flasher/check_flasher.py --port 8790   # без человека: headless Chrome
```

`check_flasher.py` проверяет страницу, затем открывает `flasher/selftest.html`. Самотест сверяет
шим со спецификацией Web Serial и прогоняет esptool-js: сброс, stub, flash ID, сброс в прошивку.
Flash он не пишет. Самотест можно открыть и руками, кнопка «Запустить».
`--flasher-image FILE --flasher-version VER` предлагает на странице другой merged-образ.

Проверено 2026-09-29 во встроенном браузере приложения (Chromium 152), движок `nickoscope/usj`,
`--cpi 2.45`:
- **A. Пустой чип, со стиранием.** События: сброс 0x15 с флагом, страп 0x3 (download), stub.
  Стирание и запись 2 361 936 байт заняли 3,8 с времени двойника. Затем сброс 0x15 без флага,
  страп 0xf, `SPI_FAST_FLASH_BOOT`, прошивка. Improv ответил «AnimatedPixelClock 2.7.3», в списке
  сетей была NickoTwin, итог «Device connected to the network!». В консоли прошивки:
  `WiFi Connected!`, IP 10.0.2.15, NTP. Портал ответил через проброс `--http`.
  `verify`: вне data-разделов flash совпадает с образом.
- **B. Поверх прошивки, без стирания.** Запись и `verify` прошли. Но Wi-Fi пришлось вводить
  заново: Full.bin закрывает NVS (0x9000–0xDFFF) байтами 0xFF, а stub пишет все байты образа.
  LittleFS (с 0x910000) и app1 лежат вне образа и сохраняются. На плате должно быть так же:
  это вывод из байтов образа, на плате не проверено.
- **C. «Logs & Console» → «Reset Device».** Сброс 0x15 без флага, прошивка стартует.
- **Чужая вкладка.** Пока порт открыт, вторая вкладка получает NetworkError, первая работает
  дальше. `panel.html` показывает «USB занят прошивальщиком».
- **Вкладка закрыта при удержании в сбросе** (RTS=1, DTR=0). Движок отпускает чип, прошивка
  загружается.
- **После перезапуска без `--blank`** двойник грузится со своего flash и подключается к точке.
- **Монитор порта (раздел 04).** Читает консоль двойника.

Ограничения:
- **Адрес с `index.html` на конце.** Каталоги движок не отдаёт, `/flasher/` ответит 404.
- **Нужен интернет.** ESP Web Tools и esptool-js грузятся с unpkg.com.
- **Пустой чип шумит.** Он без конца печатает `invalid header`: ROM без прошивки ведёт себя так же,
  но stdout растёт быстро.
- **Не всё видно странице.** После сброса по окончании прошивки ESP Web Tools сразу закрывает порт.
  Событие `release` этого сброса остаётся только в журнале движка, на страницу не приходит.
- **Метка «USB занят прошивальщиком» ставится только при смене владельца порта.** Движок сообщает
  её в `/ws` в момент, когда порт берут или отпускают. Поэтому `panel.html`, открытая уже во
  время прошивки, метку не покажет.

## Known differences from the panel

- **Timing.** By default an instruction takes one cycle. The panel's PSRAM and cache latency are not
  modelled, so Lua runs faster in the twin than on the panel. The engine's approximate timing mode
  (`-- --approximate-timing --approximate-cache --approximate-memory N`) is slower and still
  uncalibrated.
- **Radio.** The Wi-Fi is a virtual AP with a NAT to the Mac's network. Nothing reaches the twin
  from outside unless it is forwarded.
- **Peripherals not modelled:**
  - no microphones (I2S RX) and no SHTC3; the firmware treats both as absent;
  - no SD card.
- **Optics.** Glow, dot size and white balance on the page are a look. They are not calibrated
  against photos of the panel.
