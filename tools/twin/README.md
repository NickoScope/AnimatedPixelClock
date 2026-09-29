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
tools/twin/twin.py wifi NickoTwin twin-demo-2026     # the virtual AP, and what Improv hands over
tools/twin/twin.py run --fresh --provision --web 8790 # a new chip; Improv at boot; http://127.0.0.1:8790/panel.html
tools/twin/twin.py run --web 8790                     # later runs: the chip remembers
tools/twin/twin.py run --seconds 30 --png shot.png    # headless, as fast as the Mac allows
tools/twin/twin.py flash firmware.bin                 # an app image at 0x10000, like esptool
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
sudo tar Cxzvf / socket_vmnet-1.2.2-arm64.tar.gz opt/socket_vmnet
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
