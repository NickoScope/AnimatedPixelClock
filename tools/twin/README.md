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
- **The firmware:** a new chip is written with the release in `docs/firmware/latest` (its merged Full.bin; the flasher page offers the same release in its four parts), or with `TWIN_IMAGE`. The chip's own flash file wins over any image after that, so an OTA or a flash stays.
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

## Блок «Этот экран»

Под изображением панели страница показывает, что сейчас на экране: название, что он показывает, управление (ручка, пульт — подписи как на пульте страницы, портал, Home Assistant) и функции; свёрнутое «Везде» — общие действия на любом экране. EN · RU, как вся страница.

- **Встроенные экраны прошивки** описаны в `tools/twin/screens.json` (у каждой строки `source` — файл и строки кода прошивки). `build_web` кладёт его рядом с `panel.html`. Нет файла (движок без прошивки проекта) — нет блока.
- **Lua-эффект описывает себя сам**: строки `@name`, `@about`, `@control`, `@function` в шапке его скрипта (формат — `gallery/README.md`, «What a screen says about itself»). Страница читает шапку со скрипта на устройстве, `GET /api/lua/source?name=…`, поэтому описание появляется вместе с загрузкой скрипта и исчезает вместе с его удалением. Если у прошивки нет этого маршрута (или у него нет заголовка `Access-Control-Allow-Origin`), страница показывает имя эффекта и подсказку, что описание есть в шапке скрипта.
- **Какой экран сейчас**: страница раз в 1,5 с (и сразу после поворота или нажатия на странице) спрашивает `GET <портал>/api/panel`; у этого ответа есть `Access-Control-Allow-Origin: *`. Для заставки ещё `/api/clips`, для Lua-страницы — `/api/lua` (какой файл у эффекта). Всё только GET.
- **Где портал**: из строки консоли `IP Address:`, иначе `127.0.0.1:8080`. Если двойник запущен с другим `--http` или страница открыта после загрузки: `panel.html?portal=18131` (порт на этом же хосте) или `?portal=192.168.4.21`. Сам портал открывайте по `127.0.0.1` или IP, не по `localhost`: с выпуска 2.7.9 экспорт, значения портала и рынок отвечают 403 на имя хоста, которое не IPv4 и не `.local`.

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

### Синхронизация с панелью

Переключатель «Синхронизация с панелью» держит двойника и одну панель как одно устройство. Экран,
настройки, эффекты Lua и прошивка переносятся в обе стороны, после двух подтверждений. Как это
устроено и на какие строки прошивки опирается, написано в шапке `tools/twin/app/SyncEngine.swift`.
Адрес панели вручную — IP или `имя.local`, не `localhost` (см. «Настройки панели на двойника»).
Ключи вкладки «Keys» не переносятся, у каждого устройства свои. На двойнике выключены климат для HA
и запрос табло рейсов у HA (`fbAskHa`, прошивка 2.7.13): это переопределения владельца, на панель они
не переносятся.

Согласие на синхронизацию запоминается (версия 1.4, пункт 15 владельца).
- Раньше согласие жило только в памяти приложения, и после перезапуска синхронизация снова задавала
  оба вопроса. Теперь, когда отвечены оба окна, согласие сохраняется в `sync-state.json` вместе с
  состоянием пары. Хранится оно как HMAC от пары (MAC панели и MAC двойника) под тем же ключом из
  Связки ключей, что и дайджесты настроек, поэтому для другой пары его в файл не вписать.
- Приложение перезапустили, переключатель остался включён: окон нет. Синхронизация продолжается сама,
  изменения панели за время перерыва уходят на двойника. Если за перерыв менялся двойник (настройки,
  эффекты, железные настройки панели на нём), задаётся вопрос о направлении, как и раньше.
- Переключатель выключили (окно, меню или `defaults write … syncEnabled -bool NO`): согласие из файла
  забывается, при следующем включении снова два окна. Выход из приложения (⌘Q, закрытие окна, SIGTERM)
  выключением не считается: переключатель и согласие остаются.
- Новая пара или запуск с выключенным переключателем — два окна, как было.
- Первый запуск 1.4 поверх состояния 1.3. Версия 1.3 согласия не хранила. Если переключатель включён
  при запуске и в `sync-state.json` лежит выровненное состояние именно этой пары (оба MAC совпадают),
  согласие считается данным, окон нет. Если за перерыв менялся двойник, будет вопрос о направлении.
  Поэтому ночное обновление приложения не требует кликов владельца. Засчитывается это один раз: файл
  тут же переписывается в формат 1.4 — с согласием или без. Выключение переключателя тоже переписывает
  файл 1.3 в формат 1.4 без согласия, так что после выключения при включении всегда будут оба окна.
- Если Связка ключей ключа не даёт, ничего не сохраняется, и каждый запуск задаёт оба вопроса.

Эффекты Lua.
- Эффект на обеих сторонах один по имени на его заставке: `flow.lua` и `FLOW.lua` оба показывают FLOW,
  и два таких скрипта прошивка на одном устройстве не держит. Имя файла (`uploaded.scripts[].name` в
  `GET /api/lua`) сравнивается точно. Если имена отличаются только регистром, файл двойника получает имя
  как на панели: удаляется и загружается заново со своими же байтами, переключатель обхода
  возвращается как был. Список эффектов идёт в порядке файлов, поэтому порядок страниц становится как на
  панели (30.09: FLOW, KALEIDOSCOPE, NEBULA на панели и flow, kaleidoscope, nebula на двойнике, WARP на
  странице 29 и 26). Панель не переименовывается никогда.
- Содержимое скриптов (версия 1.4.1, по замерам живой пары 01.10 05:44–06:06). `GET /api/lua` даёт только
  имена и размеры, и правку, которая не меняет длину скрипта, видно лишь по его SHA-256 из
  `GET /api/lua/source`. Каждое такое чтение держит `loop()` панели (большие эффекты — до 3,8 с). В 1.4
  панель читалась пачкой: все 35 скриптов подряд раз в 5 минут. Смена экрана тогда ждала 15 с, а свободная
  память панели упала с 38184 до 28128 Б. Двойник читался весь раз в минуту, около 1,2 МБ.
  Теперь скрипты читаются по одному: у каждой стороны не чаще раза в 8 с, и каждый скрипт заново примерно
  раз в 5,5 мин (35 скриптов — по одному раз в 9,4 с). Первым читается скрипт, который появился в списке
  или изменился в размере. Пока его содержимое не прочитано, он не сравнивается: это ещё не изменение и не
  удаление. Хеши обеих сторон хранятся в `sync-state.json`, поэтому при запуске без вопросов
  (переключатель не выключали) пачки чтений нет: скрипт того же размера считается прочитанным и
  перечитывается по очереди. Двойник работает только вместе с приложением, так что за перерыв его
  скрипты измениться не могли. Для любого вопроса (окно показывает различия) обе стороны читаются
  целиком. Первый запуск 1.4.1 поверх состояния 1.4 хешей ещё не знает: все скрипты читаются по
  очереди, по одному раз в 8 с, и до своего чтения не сравниваются.
- Скрипт запрашивается по имени файла той стороны, где он лежит. Если устройство перечисляет скрипт,
  а `GET /api/lua/source` отвечает на него 404, это пишется в журнал один раз, эффект сравнивается по
  размеру, и снова скрипт не запрашивается, пока не изменится его размер. Так было с LA GIOCONDA на
  панели 30.09 21:23–22:58: загруженный скрипт `LA_GIOCONDA`, 404 в каждом раунде.
- Эффект, который принимающая сторона не берёт (её загрузка отвечает 400 на сам скрипт: проверки,
  пробный прогон, например «too slow for the panel: 4 of 4 frames over 500 ms», или нет места),
  пропускается. Запись в журнал одна, выравнивание доходит до конца, в окне направления он стоит в
  строке «Не переносится». Снова он отправляется, только когда его содержимое на исходной стороне
  изменится (при нехватке места ещё когда на принимающей стороне станет меньше скриптов). Отказы
  хранятся в `sync-state.json`.
- Если на двойнике человек загрузил эффект и список перенумеровался, страница двойника сама сдвигается
  на другой эффект. Это не выбор человека: на панель такой сдвиг не переносится, двойнику
  возвращается страница панели.

Каждая запись синхронизации (все POST, `/api/display/*`, `/api/ir/fn`, `/api/log`, загрузки эффектов
и прошивки) идёт с заголовком `X-Twin-Sync: 1`. Прошивка 2.7.13 помечает такие изменения в своих
событиях как `"by":"sync"`, а не как действие человека. Чтения заголовка не несут.

Карусель у двух устройств одна (правило владельца 2026-09-30 19:13).
- Её настройки переносятся в каждом раунде экрана, раз в 3 с, в обе стороны: включена ли она, время
  простоя, слот, «все стили», какие страницы и эффекты в обходе. Включили или выключили на любой
  стороне (портал, кнопка пульта, API) — через раунд так же и на другой.
- Пока карусель включена, ведёт панель, двойник повторяет её шаги. Шаг — это страница, а при «всех
  стилях» ещё и стиль часов. Шаг записывается в двойника как собственная запись синхронизации:
  обратно он не уходит и за действие человека не считается. Запись держит карусель двойника на время
  простоя, поэтому сама она не шагает.
- Панель ведомой не бывает. Каждый стиль, записанный в неё, через 2,5 с сохранялся бы во флеш
  (`clock_style.cpp` 43-50, 136-142), а своя карусель панели стиль только показывает. Поэтому ход
  карусели двойника в панель не пишется никогда.
- Шаг панели двойник повторяет примерно за секунду. Из `/api/panel` панели известно, через сколько
  секунд будет её шаг (`nextS`). Синхронизация читает панель сразу после этого момента и тут же пишет
  шаг в двойника. Такое чтение заменяет очередной раунд, так что панель читается почти так же часто,
  как раньше. Когда панель под нагрузкой, такого чтения нет.
- Действие человека на любой стороне (ручка, пульт, портал) переносится как раньше. Обе карусели
  стоят время простоя и идут дальше вместе, первой панель. Пока человек у двойника (его карусель
  тронули не синхронизацией или ручкой открыта страница), шаги панели его экран не перебивают.
- Перезагрузку стороны видит любое чтение её uptime (`/api/status`, `/api/info`), а не только раунд
  экрана. Раунд, который это чтение застало, останавливается, и первой обрабатывается перезагрузка:
  сброшенный экран — ничьё изменение, на панель ничего не пишется, двойник берёт экран панели. Потом
  идёт раунд экрана, за ним раунд настроек.
- Потерянная запись. Синхронизация записала настройку в двойника, а он перезагрузился раньше, чем
  успел её сохранить: модуль пишет в NVS через 2,5 с, а старая загрузка двойника не ответила и
  через 4 с после записи. Если после перезагрузки двойник снова показывает значение, которое было
  до записи, это потерянная запись: значение снова берётся с панели. Всё остальное, что изменилось
  на двойнике, — его собственное изменение (например, выбор человека в портале, сохранённый до
  перезагрузки), и оно переносится на панель как обычно.
- Запись в ведущую панель сдвинула её страницу (например, перенумеровался список эффектов). Страницу
  ей не возвращают: показ задержал бы её карусель на время простоя, а двойник и так идёт за панелью.
- Ошибка или устройство не отвечает: следующий раунд экрана через 5 с, до него ничего не читается —
  ни шаг карусели, ни эффекты, ни настройки. Панель под нагрузкой: экран раз в 5 с, шаг её карусели
  отдельно не ищется, двойник догоняет её очередным раундом.
- Синхронизацию выключили: ничего не откатывается, каждая карусель дальше идёт сама.

Мгновенный перенос (версия 1.4, прошивка 2.7.13, пункты 4, 5, 6, 8, 9, 12 владельца).
- Когда оба подтверждения даны, синхронизация подписывается на события обеих сторон
  (`POST /api/sync/listen {"port"}` раз в 25 с, `X-Twin-Sync: 1`) и отписывается (`{"stop":true}`) при
  выключении и при выходе из приложения. Датаграммы обеих сторон приходят на один UDP-сокет приложения
  (порт 47261, если свободен). Прошивка без маршрута (404, до 2.7.13) или с двумя занятыми местами (409)
  опрашивается как раньше, раз в 3 с; опрос остаётся и при событиях, как запасной путь.
- Смена страницы, стиля или яркости человеком (`by` knob, ir, http) переносится сразу: датаграмма и
  есть экран той стороны, её не читают, пишут только другую сторону и берут ответ записи как её
  экран. Своя запись синхронизации (`by` sync) обратно не идёт, ночное расписание (`schedule`) не
  сравнивается, остальное (`auto`, вкл/выкл) сразу запускает обычный раунд экрана. Если в ту же
  секунду синхронизация сама писала на сторону-источник или другая сторона изменила то же самое,
  решает раунд экрана, как раньше (кто изменил позже — по `pageS`).
- Шаг карусели ведущей панели приходит датаграммой (`by` carousel) и сразу пишется в двойника, без
  отдельного чтения панели.
- Пропуск в `seq` — потерянная датаграмма: обе стороны читаются сразу, с `GET /api/panel?input=N`
  (кольцо из 8 жестов прошивки), так что потерянный жест берётся оттуда.
- Двойник без домашней сети (NAT движка) видит Мак как 10.0.2.2, и его датаграммы доходят до Мака
  только ответом на проброшенный UDP-порт (`--hostfwd udp:<udpPort>-4210`): движок отдаёт ответ тому,
  кто последним слал на этот порт. Поэтому приложение шлёт туда пустую датаграмму со своего сокета
  (прошивка пустую не читает) и просит события на этот порт. Двойник в домашней сети шлёт напрямую.
- Долгие раунды (эффекты, настройки, прошивка, чтение скрипта по расписанию) обслуживают датаграммы
  между любыми двумя своими запросами (версия 1.4.1). Если во время чтения такого раунда пришло
  изменение человека (или шаг карусели) с той стороны, которую он читает, чтение обрывается, перенос на
  другую сторону идёт сразу, и чтение отправляется заново (один раз, потом читается целиком). Чтение
  исходника по расписанию обрывается изменением с любой стороны и повторяется не раньше чем через 3 с,
  после раунда экрана, так что два чтения исходника подряд не идут. Записи и образ прошивки не
  обрываются. Так смена экрана не ждёт ни раунда, ни долгого чтения: `GET /api/lua` идёт 0,7 с на панели
  и 1,1 с на двойнике, исходник большого эффекта — до 3,8 с. В 1.4 во время раунда эффектов перенос ждал
  1,6–3,8 с. Раунд экрана так не делает: он уже прочитал оба экрана и решает по ним.
- Если датаграммы двойника не доходят, о человеке у двойника говорят его страница в приложении
  (сообщение `twinInput` из `panel.html`: поворот и нажатие ручки, кнопки пульта, BOOT, RESET) и его
  консоль (`[luafx] open …`): через мгновение читается один двойник, и его изменение переносится, а
  последнее чтение панели заменяет её чтение (не чаще раза в 0,3 с и не под нагрузкой).

Действия внутри экрана (переносятся, только пока обе стороны показывают одну страницу).
- Эффект: нажатия его кнопки (ручка, OK пульта, `POST /api/lua {"click":true}`). С 2.7.13 на обеих
  сравнивается `now.fx.clicks` — нажатия с выбора эффекта. У каждой стороны своя база (учтённые
  нажатия: свои записи синхронизации, уже перенесённые и бывшие до того, как синхронизация увидела
  эффект); всё, что выше базы, — нажатия человека, и другой стороне досылается столько же
  (`POST /api/lua {"click":true}` с `X-Twin-Sync: 1`, когда там эффект открыт, `fx.open`), с теми же
  промежутками, что у человека (двойное нажатие остаётся двойным). Считается только чтение эффекта
  самой показанной страницы (`fx.id` равен её эффекту): сразу после смены страницы устройство ещё
  сообщает прежний эффект с его `run` и нажатиями. Если страницу поставила синхронизация или карусель,
  нажатия нового эффекта до первого такого чтения ничьи и не переносятся (как любой жест на странице,
  которой на другой стороне нет). На панель уходят только нажатия человека на двойнике: эффект,
  переоткрытый на панели (загрузка, удаление), до счёта двойника не досчитывается. Двойника, чей
  эффект открылся заново один, на странице от синхронизации (перезапуск, возврат со страницы
  человека), доводят до счёта панели. Эффект, не открывшийся за 6 с (LUA ERROR), больше не опрашивается:
  нажатия для этого `run` не переносятся, в журнале одна строка, пока он не откроется или не
  переоткроется; панель под нагрузкой ради этого не читается совсем. Эха нет, потерянная датаграмма
  досчитывается по счётчикам. Прошивка без `now.fx`: мост по `/api/knob` (прирост click + long на той
  же странице, читается только тогда).
- Мировые часы и табло рейсов: вход — `GET /api/ir/do?fn=ok`, повороты — `fn=cw|ccw` (не чаще раза в
  0,35 с, иначе прошивка сливает нажатия), выход — показ той же страницы. Табло рейсов — не пока
  двойник держит ZZZZ.
- Поезда — так же, только если на обеих табло показывают один список (`/api/railboard` list и knob).
- Медиа (каждое действие ушло бы в Home Assistant дважды), яхты, внутренний шаг рынков — не
  переносятся, одна запись в журнале. Уведомления и 3D забирают жест раньше, чем он учтён: переносить
  нечего.

## Веб-прошивальщик

Двойника прошивает та же страница, что и панель: ESP Web Tools 10.4.0 с esptool-js 0.6.0 внутри.
Сброс в режим загрузки по DTR/RTS, stub, стирание, запись, сброс в прошивку и Improv Wi-Fi идут
через настоящий ROM двойника и его USB-Serial/JTAG.

- **Как устроено.** `twin.py run --web PORT` при каждом запуске собирает `state/web/`: страницы
  движка и `flasher/`. `flasher/` — копия `docs/` (index.html, flasher.js, styles.css, img/), а в
  `flasher/firmware/latest/` лежат части выпуска, которые пишет страница: загрузчик (0x0), таблица
  разделов (0x8000), otadata (0xE000) и приложение `OTA_ONLY_…` (0x10000), с `VERSION` и
  `SHA256SUMS.txt`. Имена и адреса те же, что в манифесте `flasher.js`; если `flasher.js` попросит
  другие файлы, сборка остановится (`FLASHER_PARTS` в `twin.py`), а не отдаст странице 404. Каждая
  часть сверяется с `SHA256SUMS.txt` выпуска и с тем же куском его Full.bin, так что прошивальщик
  и новый чип движка получают одну и ту же прошивку. Сам Full.bin в копию не кладётся: страница
  его больше не пишет (его 0xFF поверх NVS стирали настройки и при установке без стирания), он
  нужен только движку для нового чипа. В `index.html` четыре правки, каждая по якорю, который
  обязан найтись ровно один раз:
  - первым в `<head>` подключается шим `flasher/twin-serial.js`, за ним `flasher/twin-lang.js` —
    язык страниц двойника;
  - ESP Web Tools закреплён на 10.4.0: в `docs/` стоит `@10` или, с main c9acf5f, `@10.4.0`; любая
    другая 10.x тоже заменяется на 10.4.0, другая старшая версия останавливает сборку;
  - к заголовку добавлено «Twin · » или «Двойник · », по языку;
  - сверху строка о том, чья это страница, с переключателем EN · RU и ссылкой на панель
    (`../panel.html?lang=…`).

  Сама `docs/` не меняется: публичная страница остаётся английской, язык меняют только строка
  двойника, заголовок, выбор порта и самотест.
- **Язык.** `en` или `ru`, по умолчанию `en`. Параметр `?lang=en|ru` главнее всего и
  запоминается в `localStorage['twin-lang']`; без него берётся запомненный язык. Переключатель
  EN · RU и `window.twinSetLang(lang)` (его зовёт приложение) меняют язык сразу, без перезагрузки.
- **Шим.** Это `navigator.serial` с одним портом 303A:1001 поверх `ws://…/usj`, протокол движка.
  Открытие и закрытие порта линии DTR/RTS не трогают. Каждый `setSignals` передаёт пару линий,
  DTR раньше RTS. По кнопке Install появляется выбор (по-английски Twin, Board over USB…, Cancel):
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

`check_flasher.py` проверяет страницу и язык (плашка, выбор порта, самотест), затем открывает
`flasher/selftest.html`. Самотест сверяет шим со спецификацией Web Serial и прогоняет esptool-js:
сброс, stub, flash ID, сброс в прошивку. Flash он не пишет. Самотест можно открыть и руками,
кнопка «Запустить» (Run).
`--flasher-image FILE --flasher-version VER` предлагает на странице другой merged-образ: `twin.py` режет
его на те же четыре части (длины загрузчика и приложения — по заголовкам их образов, otadata — по
таблице разделов) и отказывается, если раскладка не та, что пишет страница.
`check_flasher.py` проверяет и это: каждая часть из манифеста отдаётся (HTTP 200).

Проверено 2026-09-29 во встроенном браузере приложения (Chromium 152), движок `nickoscope/usj`,
`--cpi 2.45`:
- **A. Пустой чип, со стиранием.** События: сброс 0x15 с флагом, страп 0x3 (download), stub.
  Стирание и запись 2 361 936 байт заняли 3,8 с времени двойника. Затем сброс 0x15 без флага,
  страп 0xf, `SPI_FAST_FLASH_BOOT`, прошивка. Improv ответил «AnimatedPixelClock 2.7.3», в списке
  сетей была NickoTwin, итог «Device connected to the network!». В консоли прошивки:
  `WiFi Connected!`, IP 10.0.2.15, NTP. Портал ответил через проброс `--http`.
  `verify`: вне data-разделов flash совпадает с образом.
- **B. Поверх прошивки, без стирания** (тогда страница ещё писала Full.bin с 0x0). Запись и
  `verify` прошли. Но Wi-Fi пришлось вводить заново: Full.bin закрывает NVS (0x9000–0xDFFF)
  байтами 0xFF, а stub пишет все байты образа. LittleFS (с 0x910000) и app1 лежат вне образа и
  сохраняются. Исправлено в main 76c5ebd: страница пишет части, см. D.
- **C. «Logs & Console» → «Reset Device».** Сброс 0x15 без флага, прошивка стартует.

Проверено 2026-10-01 на частях 2.7.13 (копия из `build_web`, движок приложения 1.3, headless Chrome
154, ESP Web Tools 10.4.0; щелчки мышью, порт перехвачен и разобран по командам esptool):
- **D. Поверх 2.7.11 (работала из app1), без стирания.** Ровно четыре `FLASH_DEFL_BEGIN`: 0x0 на
  14 064 байта, 0x8000 на 3 072, 0xE000 на 8 192, 0x10000 на 2 207 520; команд стирания нет,
  распакованные данные побайтно равны частям. После перезагрузки 2.7.13 из app0, Wi-Fi без Improv,
  имя, эффекты FLOW и NEBULA, свой город и настройки карусели на месте; в NVS те же 183 ключа, и
  значения совпали, кроме калибровки радио `phy/cal_*`: она привязана к MAC, а экземпляр шёл с
  другим MAC, чем снимок. app1 и LittleFS не тронуты.
- **E. Новый чип (весь flash 0xFF), со стиранием.** `ERASE_FLASH` и те же четыре части. 2.7.13
  из app0, NVS по умолчанию (60 ключей), LittleFS отформатирована, Improv отвечает «готов принять
  Wi-Fi» под именем по умолчанию. Вне NVS flash совпадает с Full.bin — тем, с чего стартует новый
  чип движка.
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

Панель и двойника указывайте MAC-адресом, именем или адресом `127.0.0.1:порт` либо IP, но не
`localhost`: выпуск 2.7.9 отвечает 403 на `/api/export`, `/api/portal` и `/api/market`, если в
заголовке Host не IPv4-адрес и не имя `.local` (`web.cpp` 2510-2523).

Секреты, идентичность и состояние не переносятся. Значения секретов нигде не печатаются.
После переноса действуют переопределения владельца. От 2026-09-29 22:50: климат для HA на двойнике
выключен, чтобы в HA не было второго набора сущностей. От 2026-09-30 22:40: `fbAskHa` на двойнике
выключен (прошивка 2.7.13), см. ниже. Табло поездов по-прежнему публикует станцию
при каждом подключении к брокеру. Настройки, которая это отключает, в прошивке нет. Поэтому после
смены станции на панели запустите перенос ещё раз. Подробности и ссылки на код — в докстринге `sync.py`.

Платные экраны двойник больше не выключает (решение владельца 2026-09-30 19:35). Страницы trains
и flights, их место в обходе и выбранный аэропорт табло рейсов переносятся как всё остальное. Свой
аэропорт ZZZZ «NO REQUESTS», который добавляло прежнее переопределение, с двойника удаляется.
Ключи AeroAPI, RTT и AIS у каждого устройства свои: их вводят во вкладке «Keys» портала
(`/api/keys`). Ни `sync.py`, ни синхронизация в приложении этот маршрут не читают и не пишут; его GET
и так говорит только, сохранён ли ключ. Без ключа двойник сам платных запросов не делает: выборка
AeroAPI останавливается до счёта и до запроса (`aero_direct.cpp` 725, на экране NO KEY), у RTT так же
(`rtt_direct.cpp` 737). Платного запроса за двойника не делает и Home Assistant. Табло без ключа
AeroAPI и с брокером MQTT в NVS просит у HA табло встроенного аэропорта (`fb_mqtt.cpp` 103-136, не
чаще раза в 15 минут на аэропорт и направление и не больше 12 раз в час, `fb_mqtt.cpp` 24-45), и HA
берёт его с AeroAPI ключом владельца. Владелец 2026-09-30 22:40: двойник без своего ключа так
просить не должен. В прошивке 2.7.13 для этого есть `fbAskHa` (только `/api/export` и `/api/import`):
выключенный, он запросы в HA не шлёт, а табло, уже лежащие у брокера, по-прежнему приходят. И
`sync.py`, и синхронизация в приложении держат его на двойнике выключенным; значение сохраняется в NVS
и остаётся, когда синхронизация выключена. С версии 1.4.1 (пункт 16 владельца) приложение выключает
`fbAskHa` при каждом запуске двойника, как и даёт ему имя, независимо от переключателя синхронизации:
`POST /api/import {"fbAskHa":false}` с `X-Twin-Sync: 1`, только если `fbAskHa` есть в его `/api/export`.
Запись в `sync.log` с пометкой `override`. `fbAskHa` не сравнивается и не переносится ни в одну
сторону. Если прошивка двойника старше и `fbAskHa` в ней нет, а брокер задан и своего ключа AeroAPI нет,
табло двойника держится на своём аэропорту ZZZZ «NO REQUESTS»: свой аэропорт у HA не запрашивают
(`fb_mqtt.cpp`, mayAsk). Аэропорт панели на двойника тогда не переносится, а ZZZZ двойника на панель.
Когда у двойника появится свой ключ или прошивка с `fbAskHa`, он получит аэропорт панели, а ZZZZ
удалится. Так делают и `sync.py`, и синхронизация в приложении.

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

- **Waveshare**, for the panels and the board the twin copies: two [RGB-Matrix-P2-64x64-B](https://www.waveshare.com/rgb-matrix-p2-64x64.htm) panels and the [ESP32-S3-RGB-Matrix](https://www.waveshare.com/esp32-s3-rgb-matrix.htm) board, with its [board support package](https://github.com/waveshareteam/ESP32-S3-RGB-Matrix) and schematic.

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
- The authors of the ideas, algorithms, data and fonts behind the effects: [CREDITS.md](../../CREDITS.md).

The Mac app bundles the esp32sim engine (MIT), the ESP32-S3 ROM (Apache-2.0) and the firmware (MIT); their
notices are in the app (About) and in `tools/twin/app/NOTICE.md`.
