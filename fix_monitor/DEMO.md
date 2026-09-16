# Demo — uçtan uca kurulum

İki yol var. Hangisini seçeceğin ne göstermek istediğine bağlı.

| | Kurulum | Ne gösterir | Süre |
|---|---|---|---|
| **A. Sadece collector** | cmake + g++ | Toplama, parse, state, event store | ~2 dk |
| **B. Tam yığın** | Docker | Yukarıdakiler + Prometheus + Grafana dashboard | ~5 dk |

---

## A. Sadece collector (Docker'sız)

### Kurulum

```bash
# Debian/Ubuntu
sudo apt install -y g++ cmake make python3

# sqlite opsiyonel: yoksa CMake indirip statik gömer
sudo apt install -y libsqlite3-dev
```

macOS: `brew install cmake python3` (sqlite zaten sistemde var).

### Çalıştır

```bash
./demo.sh          # 40 saniyelik koşu
./demo.sh 120      # daha uzun
```

Script sırayla: derler, selftest'i koşturur, temiz bir demo ortamı kurar,
collector'ı başlatır, log üretecini çalıştırıp arızaları enjekte eder, sonra
session durumunu, metrikleri ve event store'dan incident timeline'ını basar.

Terminalde çalıştırırsan collector açık kalır, Prometheus'u `localhost:9109`'a
yönlendirebilirsin. Script içinden çağırırsan kendiliğinden kapanır
(`DEMO_KEEP_RUNNING=0` ile zorlanabilir).

### Beklenen çıktı

```
--- key metrics (/metrics) ---
fixmon_business_rejects_total{session="FIX.4.4:BROKER1->VENUEX",reason="2"} 1
fixmon_disconnects_total{session="FIX.4.4:BROKER1->VENUEX"} 1
fixmon_heartbeat_timeouts_total{session="FIX.4.4:BROKER1->VENUEX"} 1
fixmon_seq_gaps_total{session="FIX.4.4:BROKER1->VENUEX",src="event_log"} 1
fixmon_seq_gaps_total{session="FIX.4.4:BROKER1->VENUEX",src="message_log"} 1
fixmon_session_up{session="FIX.4.4:BROKER1->VENUEX"} 1.000000
fixmon_store_rows_written_total 517
fixmon_events_dropped_total 0

--- incident timeline from the event store ---
  17:38:40  event_log    seq_num_too_high       MsgSeqNum too high, expecting 65 but received 71
  17:38:44  message_log  Reject                 Required tag missing
  17:38:47  message_log  ExecutionReport        Instrument not tradable at this time
  17:38:47  message_log  BusinessMessageReject  Unknown security
  17:38:51  event_log    heartbeat_timeout      Test Request timed out.
  17:38:53  event_log    disconnected           Socket exception, connection reset by peer
  17:38:54  event_log    reconnect_attempt      Attempting to reconnect in 5 seconds
```

Demoda göstereceğin nokta bu son blok: `source` sütununda iki kaynağın tek
zaman ekseninde birleşmesi. Mesaj logu reject'leri veriyor, event logu
disconnect'in **sebebini** veriyor. Hiçbiri tek başına bu tabloyu üretemez.

---

## B. Tam yığın (Docker)

### Kurulum

Sadece Docker gerekiyor — derleyici, sqlite, Python hiçbiri host'ta lazım değil.

```bash
docker --version
docker compose version     # v2 varsa bu çalışır
docker-compose --version   # yoksa v1 kurulu demektir
```

**Compose v1 ve v2 farkı önemli.** Komut adı farklı (`docker compose` vs
`docker-compose`) ve v1 dosya formatı 3.8'i reddediyor. `docker-compose.yml`
bu yüzden **format 3.7** kullanıyor; ikisiyle de çalışıyor. v2, `version`
anahtarı için zararsız bir "obsolete" uyarısı basar, görmezden gel.

Dosya iki parser'la da doğrulandı: `docker-compose config` (v1.25.0) ve
`docker compose config` (v2).

v1 kullanıyorsan makineni v2'ye geçir. v1 2019'dan kalma, Python tabanlı ve
artık bakımı yapılmıyor:

```bash
mkdir -p ~/.docker/cli-plugins
ARCH=$(uname -m)   # x86_64 veya aarch64
curl -SL "https://github.com/docker/compose/releases/latest/download/docker-compose-linux-${ARCH}" \
  -o ~/.docker/cli-plugins/docker-compose
chmod +x ~/.docker/cli-plugins/docker-compose
docker compose version
```

Tek binary, ~32 MB, eski `docker-compose`'u silmene gerek yok — yan yana
durabilirler. Sistem geneli istiyorsan `~/.docker/cli-plugins` yerine
`/usr/local/lib/docker/cli-plugins`.

**Ama repodaki `version: "3.7"` satırını silme.** v2'de sadece bir satır
kozmetik uyarı üretiyor; karşılığında dosya eski `docker-compose` kurulu her
makinede çalışıyor. Orta ölçekli broker'ların çoğu eski Ubuntu LTS koşuyor ve
apt'ten gelen 1.x ile geliyor — müşteri sunucusunda açılmayan bir compose
dosyası, bir uyarı satırından çok daha pahalı.

### Çalıştır

```bash
cd deploy
docker compose up --build      # Compose v2
docker-compose up --build      # Compose v1
```

İlk derleme birkaç dakika sürer ve **bu sırada `loggen` çoktan `Up` görünür,
`fixmon` ise henüz ortada yoktur**. Başka bir terminalde `docker compose ps`
çalıştırıp tek servis görürsen bu bir arıza değil, build hâlâ sürüyordur.
Yalnız image'ı kurmak için: `docker compose build fixmon`.

Ardından:

| Servis | Adres | Not |
|---|---|---|
| Grafana | http://localhost:3000 | admin / admin, anonim görüntüleme açık |
| Prometheus | http://localhost:9090 | Alerts sekmesinde kurallar görünür |
| Collector | http://localhost:9109/metrics | ham metrikler |
| Session JSON | http://localhost:9109/sessions | debug |

Grafana'da dashboard ve datasource provisioning ile geliyor, elle kurulum yok.
**FIX Sessions** ve **FIX Session Discovery** dashboard'ları hazır gelir.

Dört servis var:

- `loggen` — FIX engine'i taklit ediyor, paylaşılan volume'a QuickFIX formatında
  log yazıyor, döngüde çalışıyor
- `fixmon` — logları **read-only** mount ile okuyor (prod'daki durumla aynı:
  loglar engine'in, collector sadece okur)
- `prometheus` — collector'ı 10 saniyede bir scrape ediyor, alert kurallarını
  yüklüyor
- `grafana` — datasource + dashboard önceden tanımlı

Collector'a hiçbir yerde session tanımlanmıyor: `fixmon.docker.ini` içinde
`[session]` bloğu yok. Session'ı da, log dosya adlarını da engine'in kendi
`quickfix.docker.cfg` dosyasından çıkarıyor. İlk log satırları bunu söylüyor:

```
config: imported FIX.4.4:BROKER1->VENUEX from /etc/fixmon/quickfix.cfg
session: FIX.4.4:BROKER1->VENUEX  hb=30s  initiator  <- /etc/fixmon/quickfix.cfg
    messages: /logs/FIX.4.4-BROKER1-VENUEX.messages.current.log
    events  : /logs/FIX.4.4-BROKER1-VENUEX.event.current.log
    credentials ignored: Username=<redacted> Password=<redacted>
```

### Yığının sağlığını tek komutla doğrula

```bash
tools/verify_stack.sh
```

Dört container'ı, collector'ın healthcheck'ini, session keşfinin engine
cfg'sinden geldiğini, **iki log kaynağının da** bağlı olduğunu, cfg'deki
credential değerlerinin uçlarda görünmediğini, Prometheus hedefini ve iki
Grafana dashboard'unu kontrol eder. Demodan önce koştur; sahnede "No data"
görmekten iyidir.

```
discovery from the engine config
  ok    session discovered: FIX.4.4:BROKER1->VENUEX
  ok    both log sources attached (messages + event)
  ok    reading logs (70645 lines)
credentials
  ok    2 credential values from the engine cfg absent from /metrics and /sessions
  ok    no credential-shaped label names
  ok    collector reports dropping 2 credential keys
prometheus
  ok    scrape target up
  ok    12 alert rules loaded
grafana
  ok    grafana healthy
  ok    dashboard provisioned: FIX Sessions
  ok    dashboard provisioned: FIX Session Discovery

stack verified
```

`fixmon_session_log_sources` **0 ise** session yapılandırılmış ama hiçbir şey
okunmuyor demektir; diğer bütün metrikler sağlıklı görünürken tek başına bu
metrik arızayı gösterir. `1` ise resmin yarısı var (genelde event log adı
tutmamıştır).

### Credential'ın sızmadığını göstermek

```bash
grep -i password deploy/quickfix.docker.cfg    # cfg'de duruyor
curl -s --noproxy '*' localhost:9109/metrics | grep -ci 'password\|username'   # 0
```

### Ne göstereceksin

1. **Dashboard açılışı** — session LOGGED ON, mesaj oranları akıyor
2. **Keşif** — collector'a session tanımlanmadı, engine cfg'sinden buldu;
   `FIX Session Discovery` dashboard'u bunu gösteriyor
3. **Sequence health paneli** — gap'in iki kaynaktan da işaretlendiği an;
   `src` label'ı ayrımı gösteriyor
4. **Reject rate paneli** — reject kodlarına göre kırılım
5. **Session events paneli** — disconnect, heartbeat timeout, reconnect
6. **Prometheus → Alerts** — `FixSessionDown`, `FixSeqNumTooLow` kuralları
7. **Collector health paneli** — drop yok, kuyruk boş, parse edilemeyen satır
   sayısı (üreteç bilerek tanınmayan bir satır yazıyor, orada görünür)

### Arızayı canlı tetiklemek

Demo sırasında session'ı gerçekten düşürmek etkileyici oluyor:

```bash
docker compose stop loggen     # v1: docker-compose stop loggen
```

30–60 saniye içinde `fixmon_last_message_age_seconds` tırmanır, session
**STALE**'e geçer, `FixSessionStale` alert'i Pending'e düşer. Sonra:

```bash
docker compose start loggen
```

### Temizlik

```bash
docker compose down -v      # -v volume'ları da siler
# v1: docker-compose down -v
```

---

## Test edilme durumu

Dürüst olmak gerekirse:

| Parça | Durum |
|---|---|
| Collector, adaptörler, state machine, event store | Test edildi, 60+ kontrol geçiyor |
| `demo.sh` uçtan uca | Çalıştırıldı, yukarıdaki çıktı gerçek |
| Metrik isimleri ↔ dashboard/alert sorguları | Otomatik karşılaştırıldı, uyumsuzluk yok |
| Datasource uid ↔ dashboard uid | Doğrulandı |
| Tüm YAML/JSON ve compose mount yolları | Doğrulandı |
| Compose dosyası, v1.25.0 parser'ı | `docker-compose config` ile doğrulandı, geçerli |
| Compose dosyası, v2 parser'ı | `docker compose config` ile doğrulandı, geçerli (sadece `version` uyarısı) |
| **Docker Compose yığınının kendisi** | **Çalıştırıldı** — Rocky Linux 8 / WSL2, Docker 26.1.3, dört servis `Up`, `fixmon` healthy |
| `tools/verify_stack.sh` | Çalışan yığına karşı koştu, 16 kontrolün tamamı geçti |
| **Windows / MSVC derlemesi** | **Denenmedi** |

Yığın gerçekten ayağa kalktı: image derlendi, selftest build içinde geçti,
session engine cfg'sinden keşfedildi, iki log kaynağı da bağlandı, Prometheus
hedefi UP geldi ve iki dashboard da provision oldu. Yukarıdaki `verify_stack.sh`
çıktısı o koşudan alındı.

---

## Sorun giderme

**`cmake` sqlite bulamıyor** — normal, otomatik indirmeye düşecek. Ağ yoksa:
`sudo apt install libsqlite3-dev` veya
`-DFIXMON_SQLITE_PROVIDER=local -DFIXMON_SQLITE_SOURCE_DIR=...`

**Metrikler boş** — `logs/` altındaki iki dosya var mı, `fixmon.ini`'deki
yollarla eşleşiyor mu. Collector başladıktan sonra yazılan satırları görür;
mevcut içeriği de okumak için `from_beginning = true`.

**`bind: address already in use`** — host'ta o portu başka bir şey tutuyor.
En sık sebebi daha önce çalıştırdığın `./demo.sh`: interaktif modda collector
bilerek açık kalıyor ve 9109'u tutuyor.

```bash
ss -ltnp | grep 9109        # kim tutuyor
pkill -f 'build/fixmon'     # demo.sh'in bıraktığı süreci öldür
```

Ya da host portlarını değiştir — compose bunu ortam değişkeniyle kabul ediyor:

```bash
FIXMON_PORT=9110 GRAFANA_PORT=3001 PROMETHEUS_PORT=9091 docker compose up --build
```

Değişken adları compose dosyasındakilerle birebir aynı olmalı. Yanlış yazarsan
compose sessizce varsayılana düşer ve port çakışması sürer — hata vermez.

Collector'ın host portu aslında opsiyonel: Prometheus ona compose ağı üzerinden
`fixmon:9109` ile ulaşıyor. Host'tan `curl /metrics` yapmayacaksan `fixmon`
servisinin `ports:` bloğunu tamamen silebilirsin.

**Sadece `loggen` ayakta, diğer üç servis yok** — büyük ihtimalle arıza değil.
`loggen` hazır bir image çekiyor ve saniyeler içinde başlıyor; `fixmon` ise
kaynaktan derleniyor ve ilk seferde birkaç dakika sürüyor. Prometheus ve Grafana
ona bağlı olduğu için sırada bekliyorlar. Build'in hâlâ sürdüğünü şuradan
anlarsın:

```bash
docker compose logs -f fixmon      # build bitince container logları akar
docker compose build fixmon        # ya da sadece build'i izole et
```

Build gerçekten patlıyorsa hatayı bu ikinci komut açıkça gösterir. Build bittiği
hâlde servisler gelmiyorsa `docker compose up -d` ile tekrar dene.

**Grafana açılmıyor / servis kapalı** — `docker-compose ps` ile dört servis de
`Up` mı bak. Liste boşsa stack hiç kalkmamıştır; `docker-compose logs --tail=50`
sebebini gösterir. En sık neden fixmon image build'inin patlaması: `grafana`
→ `prometheus` → `fixmon` zinciriyle bağlı olduğu için biri patlayınca Grafana
hiç başlamıyor. Sadece build'i denemek için `docker-compose build fixmon`.

**"Unsupported config option for services"** — Compose v1 kullanıyorsun ve
dosyada `version` anahtarı yok demektir. Bu repodaki dosyada var (3.7); başka
bir kopya kullanıyor olabilirsin.

**Grafana "No data"** — Prometheus'ta Status → Targets, `fixmon` hedefi UP mı.
Değilse container adı çözülmüyor demektir.

**`curl localhost:9109` beklenmedik bir cevap veriyor** — kurumsal ağlarda
`http_proxy`/`https_proxy` ortam değişkenleri tanımlıysa curl localhost'a giden
isteği bile proxy'ye yollar ve proxy'nin hata sayfasını okursun. Bu yüzden
`tools/verify_stack.sh` tüm çağrılarında `--noproxy '*'` kullanıyor. Elle
denerken sen de kullan:

```bash
curl -s --noproxy '*' localhost:9109/metrics | head
```

**`docker compose logs loggen` boş görünüyor** — Python stdout'u buffer'lıyor,
üreteç çalışsa bile satırlar hemen görünmeyebilir. Logların akması gerçek bir
sağlık göstergesi değil; volume'daki dosyalara bak:

```bash
docker compose exec fixmon sh -c 'wc -l /logs/*.log'
```

**Session hep "unknown"** — `sender_comp_id` config'de logdaki tag 49 ile birebir
eşleşmeli; yön tespiti buna dayanıyor.

**`unparsed` sayacı yükseliyor** — engine'in ifadeleri kural tablosuyla
uyuşmuyor. Satırlar ham haliyle saklanıyor, kaybolmuyor:
`SELECT text FROM events WHERE session_event='unparsed'` ile bakıp
`src/event_log_adapter.cpp` içindeki tabloya kural ekle.

Demo yığınında bu sayacın **sıfırdan büyük olması normal**: `tools/gen_logs.py`
her turda bilerek tanınmayan bir satır (`Vendor-specific condition XYZ-4471...`)
yazıyor. Amaç tam olarak bu davranışı göstermek — collector bilmediği satırı
atmıyor, sayıyor ve saklıyor.

