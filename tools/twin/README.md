# tools/twin — the virtual panel

The real firmware image, unchanged, on an emulated ESP32-S3 and a model of the 128x64 HUB75 panel.
You see on the Mac what the LEDs would show. The remote and the knob are pin-level models, and the
firmware decodes them itself. Why it is built this way, and what it can never match: knowledge
base `docs/40-virtual-twin.md` (ADR-TWIN-01).

## What runs

- **Engine:** our fork of [esp32sim](https://github.com/joakimeriksson/esp32sim) (Rust, MIT):
  https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128, branch `nickoscope/twin` (see `engine/README.md`).
- **The fork adds:**
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
git clone -b nickoscope/twin https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128 ~/twin/esp32sim
cd ~/twin/esp32sim && cargo build --release
```

- **The ESP32-S3 mask ROM** comes from Espressif's esp-rom-elfs releases: `esp32s3_rev0_rom.elf` into `~/twin/rom/`.
- **The firmware:** a new chip is written with the release in `docs/firmware/latest` (the one the flasher offers), or with `TWIN_IMAGE`. The chip's own flash file wins over any image after that, so an OTA or a flash stays.
- **Symbols** (names in traces and crash reports): each build's `firmware.elf` goes to `~/twin/fw/<build>/`. `run` takes the one whose SHA-256 the booted app carries in its descriptor (`esp_app_desc_t.app_elf_sha256`), so a rebuild of the same version is never mistaken for it. Without a match the run goes on with addresses only and says so; `TWIN_ELF` names one by hand.
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

## Приложение для Мака

`tools/twin/app/` — приложение «TWIN — панель» (`TWIN Panel.app`): окно со страницей панели и кнопками «Портал», «Прошивальщик», «Перезапустить».
- Открыли приложение — двойник запускается: `twin.py run --lan --web 8790`.
- Закрыли окно или вышли (⌘Q) — двойник останавливается. Так двойник работает, только пока с ним работают (решение владельца, 29.09).
- Если двойник уже запущен из терминала, приложение показывает его, а при выходе оставляет работать.

```
tools/twin/app/build.sh                 # собрать и положить в ~/Applications (нужен только swiftc из Xcode)
defaults write com.nickoscope.twinpanel lan -bool NO     # без домашней сети (NAT, портал на 127.0.0.1:8080)
defaults write com.nickoscope.twinpanel port -int 8791   # другой порт страницы
```

Приложение запускает `twin.py` этой копии репозитория: путь записывается в приложение при сборке. Если копия переедет, соберите заново. Движок, `twin.py` и страница те же, приложение их только запускает и показывает.

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
- **Движок** — форк NickoScope/TWIN-NickoScopeMatrix-64x128, ветка `nickoscope/twin`: канал `/usj` в нём есть.
  Со старым движком без `/usj` страница откроется, но порт — нет: ESP Web Tools покажет
  «Failed to open serial port».

```
tools/twin/twin.py wifi NickoTwin twin-demo-2026   # виртуальная точка: её предложит Improv
tools/twin/twin.py run --blank --web 8790          # новый чип, пустой (ROM: invalid header); старый flash.bin уходит в state/backup/
#   http://127.0.0.1:8790/flasher/ → Connect → «Двойник» → Install AnimatedPixelClock
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
  from outside unless it is forwarded, or unless it runs with `--lan` (below).
- **Peripherals not modelled:**
  - no microphones (I2S RX) and no SHTC3; the firmware treats both as absent;
  - no SD card.
- **Optics.** Glow, dot size and white balance on the page are a look. They are not calibrated
  against photos of the panel.

## Настройки панели на двойника

`tools/twin/sync.py` переносит настройки панели на двойника одной командой. Панель он только читает (GET).
Пишет только в устройство, чей `/api/info` показывает имя `TWIN-…` и локально-администрируемый MAC.

```
tools/twin/sync.py              # план: поле, было -> станет; запросы по порядку; ничего не отправляет
tools/twin/sync.py --diff       # только различия, по видам
tools/twin/sync.py --apply      # выполнить, перечитать оба устройства, показать, что осталось
python3 -m unittest tools/twin/test_sync.py -v    # поддельные устройства, без сети
```

Секреты, идентичность и состояние не переносятся. Значения секретов нигде не печатаются.
После переноса действуют переопределения владельца от 2026-09-29 22:50: климат для HA выключен,
страницы trains и flights убраны из обхода, выбран свой аэропорт ZZZZ, для которого запросов
табло рейсов нет. Табло поездов по-прежнему публикует станцию при каждом подключении к брокеру.
Настройки, которая это отключает, в прошивке нет. Поэтому после смены станции на панели запустите
перенос ещё раз. Подробности и ссылки на код — в докстринге `sync.py`.

## В домашней сети

`twin.py run --lan` выпускает твин в домашнюю сеть как отдельное устройство: свой IP от роутера,
имя `TWIN-…` в DHCP и mDNS, `mac=02:54:57:49:4E:01` в TXT. Основание и границы — ADR-TWIN-02 в базе
знаний. Движку нужен режим `--net bridge:PATH` (esp32sim, ветка `nickoscope/bridge`).

Как устроено. Мост vmnet на Wi-Fi (`en0`) держит демон
[socket_vmnet](https://github.com/lima-vm/socket_vmnet) v1.2.2 от root. Твин подключается к его
сокету без sudo. Кадры станции уходят в сеть как есть, DHCP отвечает роутер. Встроенные в эмулятор
DHCP, DNS, NTP и NAT в этом режиме не работают, `--hostfwd` не передаётся.

Что видно в сети. В эфире и в ARP-таблицах за IP твина стоит MAC Wi-Fi этого Mac: ядро включает
MAC-NAT, когда в мост входит Wi-Fi (флаг `MACNAT` у `en0` в `ifconfig`). MAC твина виден в DHCP
(`chaddr` и option 61) и в mDNS. Прошивка v2.7.3 в твине шлёт option 61 = `01:02:54:57:49:4e:01`
и option 12 = своё имя; это проверено через `fake_vmnet.py`.

### Один раз, с sudo (владелец)

Ставить из релизного тарбола, не через Homebrew: бинарник в `${HOMEBREW_PREFIX}` может подменить
любой пользователь группы `admin` (README socket_vmnet v1.2.2, строка 190), а запускается он от root.

```
cd ~/Downloads
curl -OSL https://github.com/lima-vm/socket_vmnet/releases/download/v1.2.2/socket_vmnet-1.2.2-arm64.tar.gz
gh attestation verify --owner=lima-vm socket_vmnet-1.2.2-arm64.tar.gz
sudo tar -C / -xzvf ~/Downloads/socket_vmnet-1.2.2-arm64.tar.gz ./opt/socket_vmnet   # the archive's paths start with ./
sudo mkdir -p /var/log/socket_vmnet
sudo cp /opt/socket_vmnet/share/doc/socket_vmnet/launchd/io.github.lima-vm.socket_vmnet.bridged.en0.plist /Library/LaunchDaemons/
sudo launchctl bootstrap system /Library/LaunchDaemons/io.github.lima-vm.socket_vmnet.bridged.en0.plist
sudo launchctl enable system/io.github.lima-vm.socket_vmnet.bridged.en0
sudo launchctl kickstart -kp system/io.github.lima-vm.socket_vmnet.bridged.en0
```

Проверка (A0): `ls -l /var/run/socket_vmnet.bridged.en0` показывает `srwxrwx--- root staff`;
в `/var/log/socket_vmnet/bridged.en0.stderr` нет ошибок vmnet; `ifconfig` показывает `bridgeN`
с членами `vmenetN` и `en0`.

### Перед первым запуском

1. **Имя.** Твин должен называться `TWIN-…`, иначе `--lan` не запустится. Имя задаётся в обычном
   запуске (порт 8080 на 127.0.0.1), затем твин перезапускается:
   `curl -X POST -d '{"name":"TWIN-NickoScopeMatrix-64x128-01"}' http://127.0.0.1:8080/api/rename`
   (маршрут и правило имени: `src/web/web.cpp` 312, 689-698). `twin.py` читает имя прямо из NVS
   во флеше твина.
2. **Домашний guard.** Дома один раз: `tools/twin/twin.py lan-setup`. Он записывает IP и MAC шлюза
   по умолчанию и интерфейс в `~/twin/state/lan.txt` (права 0600, вне репозиториев). Демон
   бриджит любую сеть, в которой сейчас `en0`; с другим шлюзом `--lan` откажется.
3. **DHCP через мост (A1).** `tools/twin/lanprobe.py` отправляет через сокет один DHCPDISCOVER
   от имени твина и печатает OFFER: адрес, маску, роутер. REQUEST не отправляется, аренды нет.

### Запуск

```
tools/twin/twin.py run --lan --web 8790     # страница панели по-прежнему на 127.0.0.1:8790
```

Перед стартом `twin.py` проверяет: сокет есть и принимает соединение (иначе печатает команды
установки), сеть та же, что в `lan.txt`, имя начинается с `TWIN-`. Портал твина — по его адресу
из строки `IP Address:` в консоли, или `discover.py --mac 02:54:57:49:4E:01`. В конце прогона
движок печатает счётчики моста: `rx_ok`, `rx_filtered`, `rx_dropped`, `tx`, `tx_dropped`,
`reconnects`.

Проверить всё это без демона и без сети: `fake_vmnet.py` играет демон и роутер
(`python3 tools/twin/fake_vmnet.py fake.sock`, затем `TWIN_LAN_SOCKET=fake.sock twin.py run --lan`).

### Безопасность

HTTP API твина, как у панели, без аутентификации: любой хост сети может переименовать его,
загрузить Lua или прошивку по OTA. За гостем стоит эмулятор с доступом к домашнему каталогу. Держать
твин в сети только на время испытаний. Боевые учётки MQTT во флеш твина не класть.

### Выключить и удалить

```
sudo launchctl bootout system /Library/LaunchDaemons/io.github.lima-vm.socket_vmnet.bridged.en0.plist
sudo rm /Library/LaunchDaemons/io.github.lima-vm.socket_vmnet.bridged.en0.plist
sudo rm -rf /opt/socket_vmnet /var/log/socket_vmnet
```

Только выключить до следующих испытаний — первая строка. Включить снова — `bootstrap` и
`kickstart` из установки.

## Authors and thanks

The twin stands on other people's work. Thank you all.

- **esp32sim**, the emulator the twin runs on: [Joakim Eriksson (@joakimeriksson)](https://github.com/joakimeriksson) and
  [Alice (@aliceisjustplaying)](https://github.com/aliceisjustplaying), MIT - https://github.com/joakimeriksson/esp32sim.
  It boots the real ESP32-S3 ROM and runs Espressif's own Wi-Fi blob, which is what made a true twin possible.
  Our additions are the fork https://github.com/NickoScope/TWIN-NickoScopeMatrix-64x128 (NICKOSCOPE.md lists them).
- **AnimatedPixelClock**, the firmware the twin runs: [Keralots](https://github.com/Keralots/AnimatedPixelClock), MIT;
  the fork [NickoScope/AnimatedPixelClock](https://github.com/NickoScope/AnimatedPixelClock).
- **ESP32-HUB75-MatrixPanel-DMA** by [mrcodetastic](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA):
  the panel driver whose output the twin's HUB75 decoder reads back into light.
- **Espressif**: the ESP32-S3 mask ROM ([esp-rom-elfs](https://github.com/espressif/esp-rom-elfs), Apache-2.0),
  ESP-IDF and [arduino-esp32](https://github.com/espressif/arduino-esp32), the ESP32-S3 Technical Reference
  Manual the models follow, [esptool](https://github.com/espressif/esptool) and
  [esptool-js](https://github.com/espressif/esptool-js) (Apache-2.0).
- **ESP Web Tools** by the [ESPHome maintainers](https://github.com/esphome/esp-web-tools) (Apache-2.0): the
  web flasher; **Improv Wi-Fi** ([improv-wifi.com](https://www.improv-wifi.com/)) and the
  [Improv WiFi Library](https://github.com/jnthas/Improv-WiFi-Library) by jnthas (MIT).
- **socket_vmnet** by the [Lima project](https://github.com/lima-vm/socket_vmnet) (Apache-2.0): the twin's bridge
  to the home network.
- The firmware's libraries, which the twin runs as they are: WiFiManager (tzapu, tablatronix), ArduinoJson
  (Benoît Blanchon), Adafruit GFX, PubSubClient (Nicholas O'Leary), arduinoWebSockets (Markus Sattler),
  IRremoteESP8266 (David Conran, Mark Szabo, Sébastien Warin, Ken Shirriff and others), QRCode (Richard Moore),
  [Lua 5.4](https://www.lua.org/) (PUC-Rio).

The Mac app bundles the esp32sim engine (MIT), the ESP32-S3 ROM (Apache-2.0) and the firmware (MIT); their
notices are in the app (About) and in `tools/twin/app/NOTICE.md`.
