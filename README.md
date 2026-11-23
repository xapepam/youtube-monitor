# YouTube Monitor

Небольшой сервис на C++ для проверки скорости загрузки видео с YouTube.
Результаты доступны через HTTP API и могут использоваться в Home Assistant.

Программа получает временную ссылку на видео, загружает до 5 000 000 байт,
измеряет скорость и отдаёт результат в JSON.

Для работы **не нужны Python, yt-dlp и Node.js**.

> Получение ссылки использует внутренний API YouTube. Его поведение может
> измениться, поэтому работоспособность не гарантируется постоянно или
> для любого видео.

## Возможности

- Задание видео через переменную окружения.
- Периодическая проверка скорости.
- Настраиваемые порог низкой скорости и таймаут загрузки.
- HTTP API для Home Assistant.
- Сборка под `amd64` и `arm64`.
- Запуск в контейнере на MikroTik RouterOS.

Видео **не сохраняется на диск и не накапливается целиком в памяти**:
полученные данные учитываются и отбрасываются. Страница YouTube и ответ API
временно находятся в памяти для разбора.

## Файлы проекта

| Файл | Назначение |
|---|---|
| `youtube-monitor.cpp` | Получение ссылки и измерение скорости |
| `monitor-service.inc` | Периодические проверки, настройки и HTTP API |
| `visionos-client.inc` | Параметры клиента YouTube для сборки |
| `Dockerfile` | Многоэтапная сборка Alpine-образа |

`visionos-client.inc` нужен при сборке, но не требуется работающему
бинарнику как отдельный файл.

## Принцип работы

При запуске сервис:

1. Читает настройки из переменных окружения.
2. Получает страницу указанного видео и свежий Visitor ID.
3. Запрашивает доступные форматы видео через API YouTube.
4. Выбирает прямой видеоформат разрешением до 1080p.
5. Загружает до 5 МБ видео и вычисляет скорость.
6. Публикует результат на HTTP-адресе `/status`.
7. Ждёт заданный интервал и повторяет проверку.

Первая проверка запускается сразу. Следующая начинается через заданный
интервал **после завершения предыдущей**.

Проверка измеряет загрузку выбранного видеофрагмента, а не фактическое
разрешение на телевизоре или компьютере. Низкая скорость может привести
к снижению качества, но точного соответствия между Мбит/с и 480p нет.

## Переменные окружения

| Переменная | Обязательна | По умолчанию | Описание |
|---|---:|---:|---|
| `YTM_VIDEO_URL` | Да | — | Ссылка на видео или его ID |
| `YTM_INTERVAL` | Нет | `3600` | Пауза между проверками, секунд |
| `YTM_LOW_MBPS` | Нет | `2.5` | Порог низкой скорости, Мбит/с |
| `YTM_TIMEOUT_SECONDS` | Нет | `20` | Таймаут загрузки видео, секунд |

### `YTM_VIDEO_URL`

Поддерживаются:

```text
https://www.youtube.com/watch?v=jmRI3Ew4BvA
https://youtu.be/jmRI3Ew4BvA
jmRI3Ew4BvA
```

Выбирайте открытое видео, доступное без авторизации. Если видео удалено,
недоступно в регионе или YouTube требует дополнительной проверки, сервис
может вернуть ошибку.

### `YTM_INTERVAL`

Пауза после завершения проверки, в секундах.

Допустимый диапазон: от `1` до `86400`.

Примеры:

```text
300    — 5 минут
1800   — 30 минут
3600   — 1 час
```

### `YTM_LOW_MBPS`

Порог в Мбит/с. При успешной проверке:

- скорость **ниже** порога увеличивает `low_count`;
- скорость **равна порогу или выше** сбрасывает `low_count` в `0`.

Значение должно быть больше `0` и не больше `10000`.
Дробную часть указывайте через точку:

```text
2.5
```

### `YTM_TIMEOUT_SECONDS`

Максимальное время загрузки видеофрагмента в секундах.
Допустимый диапазон: от `1` до `3600`.

Таймаут относится к загрузке видео, а не ко всей проверке:
получение страницы и запроса к API имеет отдельные ограничения.

**Текущее ограничение:** если загрузка завершится по тайм-ауту, она будет
записана как ошибка и увеличит `error_count`, а не `low_count`.

## Локальная сборка

Понадобятся:

- компилятор C++17;
- `libcurl` и заголовки для разработки;
- `nlohmann-json`;
- `pkg-config`;
- POSIX threads.

Пример для Manjaro:

```bash
sudo pacman -S --needed base-devel curl nlohmann-json
```

Сборка:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread \
  youtube-monitor.cpp -o youtube-monitor-cpp \
  $(pkg-config --cflags --libs libcurl)
```

Запуск:

```bash
YTM_VIDEO_URL="https://www.youtube.com/watch?v=jmRI3Ew4BvA" \
YTM_INTERVAL=3600 \
YTM_LOW_MBPS=2.5 \
YTM_TIMEOUT_SECONDS=20 \
./youtube-monitor-cpp
```

Для короткого локального теста можно временно уменьшить интервал:

```bash
YTM_VIDEO_URL="jmRI3Ew4BvA" \
YTM_INTERVAL=30 \
./youtube-monitor-cpp
```

Программа работает до остановки через `Ctrl+C`.

## HTTP API

Сервис слушает порт `8000` на всех IPv4-интерфейсах контейнера или хоста.

Единственный предусмотренный маршрут:

```text
GET /status
```

Проверка локального процесса:

```bash
curl -sS http://127.0.0.1:8000/status | python -m json.tool
```

Пример успешного ответа:

```json
{
  "status": "ok",
  "speed_mbps": 42.2,
  "low_count": 0,
  "error_count": 0,
  "checked_at": "2026-11-07T01:36:55Z",
  "detail": "Загружено 5000000 байт за 0.95 с"
}
```

Значения в примере приведены для иллюстрации.

### Поля ответа

| Поле | Описание |
|---|---|
| `status` | `starting`, `ok` или `error` |
| `speed_mbps` | Скорость последней успешной проверки; при ошибке — `null` |
| `low_count` | Количество последовательных успешных проверок ниже порога |
| `error_count` | Количество последовательных ошибок проверки |
| `checked_at` | Время завершения последней проверки в UTC |
| `detail` | Сведения о загрузке или текст ошибки |

При успешной проверке `error_count` сбрасывается в `0`.
При ошибке `low_count` сбрасывается в `0`.

Счётчики находятся в памяти процесса и сбрасываются при перезапуске.

### Безопасность API

HTTP API **не имеет авторизации**. Не публикуйте порт `8000` в интернет.

Для локальной проверки Podman публикуйте порт только на loopback хоста:

```text
127.0.0.1:18000:8000
```

На MikroTik ограничивайте доступ к API доверенной сетью в соответствии с
существующей конфигурацией firewall.

### Актуальность результата

HTTP-сервис может продолжать отвечать последним результатом, если рабочий
поток проверки завис. Сам по себе `status: ok` не доказывает, что результат
свежий. При мониторинге проверяйте также `checked_at`.

## Сборка контейнера для amd64

```bash
podman build \
  --platform linux/amd64 \
  -t youtube-monitor:0.1.1-amd64 .
```

Запуск:

```bash
podman run -d --rm \
  --name youtube-monitor-test \
  -p 127.0.0.1:18000:8000 \
  -e YTM_VIDEO_URL="https://www.youtube.com/watch?v=jmRI3Ew4BvA" \
  -e YTM_INTERVAL=3600 \
  -e YTM_LOW_MBPS=2.5 \
  -e YTM_TIMEOUT_SECONDS=20 \
  localhost/youtube-monitor:0.1.1-amd64
Проверка:

```bash
curl -sS http://127.0.0.1:18000/status | python -m json.tool

Журнал и текущий расход памяти:

```bash
podman logs youtube-monitor-test
podman stats --no-stream youtube-monitor-test
Остановка:

```bash
podman stop youtube-monitor-test

Контейнер запущен с `--rm`, поэтому после остановки будет удалён.
Образ при этом останется.

Для изменения переменных контейнер нужно создать заново: переменные
окружения существующего контейнера через `podman restart` не меняются.

## Сборка контейнера для arm64

```bash
podman build \
  --platform linux/arm64 \
  -t youtube-monitor:0.1.1-arm64 .
```

Проверка архитектуры:

```bash
podman image inspect localhost/youtube-monitor:0.1.1-arm64 \
  --format '{{.Os}}/{{.Architecture}}'
```

Ожидается:

```text
linux/arm64
Если сборка на x86 завершается ошибкой `Exec format error`, можно
зарегистрировать ARM-эмулятор:

```bash
sudo podman run --privileged --rm \
  docker.io/tonistiigi/binfmt --install arm64

Затем повторить сборку. Регистрация эмулятора изменяет настройки ядра хоста
и может не сохраниться после перезагрузки.

Сохранение образа для RouterOS:

```bash
podman save \
  --format docker-archive \
  -o youtube-monitor-0.1.1-arm64.tar \
  localhost/youtube-monitor:0.1.1-arm64
```bash
ls -lh youtube-monitor-0.1.1-arm64.tar

## Установка на MikroTik

Пример для RouterOS с поддержкой контейнеров и флешкой, смонтированной как
`Storage_4Gb`.

> Перед созданием убедитесь, что имена интерфейса, списка переменных и
> каталог контейнера не используются другими объектами. Команды ниже
> предназначены для **новой установки**, не для обновления поверх старой.

### 1. Загрузка архива

Загрузите `youtube-monitor-0.1.1-arm64.tar` через WinBox в корень флешки.

Проверьте:

```routeros
/file/print where name~"youtube-monitor"
Ожидаемый путь к архиву:

```text
Storage_4Gb/youtube-monitor-0.1.1-arm64.tar

Убедитесь, что на флешке достаточно места для архива и распакованного образа.

### 2. Интерфейс

Пример использует сеть `172.22.155.244/30`:

- MikroTik: `172.22.155.245`;
- контейнер: `172.22.155.246`.

Сначала убедитесь, что подсеть свободна:

```routeros
/ip/address/print detail
/ip/route/print detail
/interface/veth/print detail
```

Если она не конфликтует с существующими сетями:

```routeros
/interface/veth/add \
    name=veth-youtube \
    address=172.22.155.246/30 \
    gateway=172.22.155.245
```

```routeros
/ip/address/add \
    address=172.22.155.245/30 \
    interface=veth-youtube
```

### 3. Переменные

```routeros
/container/envs/add \
    list=youtube-monitor \
    key=YTM_VIDEO_URL \
    value="https://www.youtube.com/watch?v=jmRI3Ew4BvA"
```

```routeros
/container/envs/add \
    list=youtube-monitor \
    key=YTM_INTERVAL \
    value=3600
```routeros
/container/envs/add \
    list=youtube-monitor \
    key=YTM_LOW_MBPS \
    value=2.5

```routeros
/container/envs/add \
    list=youtube-monitor \
    key=YTM_TIMEOUT_SECONDS \
    value=20
```

### 4. Импорт

В примере указаны DNS-серверы, которые использовались в проверенной ранее
конфигурации. При необходимости замените их на доступные в вашей сети.

```routeros
/container/add \
    file=Storage_4Gb/youtube-monitor-0.1.1-arm64.tar \
    interface=veth-youtube \
    root-dir=/Storage_4Gb/youtube-monitor \
    envlist=youtube-monitor \
    dns=95.167.167.95,95.167.167.96,77.88.8.8,77.88.8.1 \
    hostname=youtube-monitor \
    logging=yes \
    start-on-boot=yes
```

Проверьте состояние импорта:

```routeros
/container/print detail where interface=veth-youtube
Когда контейнер будет готов и остановлен, запустите его:

```routeros
/container/start [find where interface=veth-youtube]

### 5. Проверка

С самого MikroTik:

```routeros
:put ([/tool/fetch \
    url="http://172.22.155.246:8000/status" \
    output=user as-value]->"data")
Параметры контейнера и журнал:

```routeros
/container/print detail where interface=veth-youtube

```routeros
/log/print where topics~"container"
```

Текущий расход памяти показывается в `memory-current`. Он не обязательно
равен пиковому расходу во время проверки.

### Маршрутизация

Эта инструкция **не добавляет правил firewall, NAT или маршрутизации**.
Контейнер использует существующую конфигурацию роутера.

Чтобы измерять именно соединение через туннель, трафик к YouTube и
видеосерверам должен идти через нужный туннель. Также контейнеру нужен
рабочий DNS, а Home Assistant — доступ к адресу `172.22.155.246:8000`.

### Изменение видео или настроек

Остановите контейнер, измените нужное значение и запустите его снова.
Например, чтобы сменить видео:

```routeros
/container/stop [find where interface=veth-youtube]
```

```routeros
/container/envs/set \
    [find where list="youtube-monitor" and key="YTM_VIDEO_URL"] \
    value="https://www.youtube.com/watch?v=НОВЫЙ_ID"
```routeros
/container/start [find where interface=veth-youtube]

Аналогично меняются `YTM_INTERVAL`, `YTM_LOW_MBPS` и
`YTM_TIMEOUT_SECONDS`. Значения считываются при старте контейнера.

## Home Assistant

Пример REST-сенсора для контейнера с адресом `172.22.155.246`:

```yaml
rest:
  - resource: http://172.22.155.246:8000/status
    scan_interval: 60
    timeout: 10
    sensor:
      - name: YouTube test
        unique_id: youtube_tunnel_test
        value_template: "{{ value_json.status }}"
        json_attributes:
          - speed_mbps
          - low_count
          - error_count
          - checked_at
          - detail
```

Если `rest:` уже существует в `configuration.yaml`, добавьте элемент в
существующий список, а не создавайте второй раздел `rest:`.

После проверки конфигурации и перезапуска HA появится сущность
`sensor.youtube_test`, если такое имя не занято.

Для уведомления можно использовать:

- `low_count >= 3` — три последовательных низких результата;
- `error_count >= 3` — три последовательные ошибки;
- `unavailable` — Home Assistant не может получить ответ сервиса.

При интервале в один час три последовательных низких результата появятся
примерно через два часа после первого плохого измерения. Дополнительно
проверяйте свежесть `checked_at`.

## Известные ограничения

1. API YouTube и параметры клиента могут измениться.
2. Некоторые видео могут быть недоступны без авторизации.
3. Таймаут загрузки сейчас считается ошибкой, а не низкой скоростью.
4. Проверка одного видео и одного CDN-сервера не гарантирует одинаковую
   скорость на других устройствах.
5. Счётчики сбрасываются при перезапуске процесса.
6. HTTP API не защищён авторизацией.
7. Если рабочий поток зависнет, HTTP API может показывать старый результат.
8. Сетевой маршрут через туннель зависит от конфигурации роутера.
