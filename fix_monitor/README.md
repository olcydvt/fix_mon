# fixmon

QuickFIX loglarından beslenen FIX session monitörü. İki ayrı adaptör, tek bir
normalize edilmiş event akışı, Prometheus metrikleri ve SQLite event store.

Sniffing yok, proxy yok, order path'ine dokunma yok. Sadece log okuma yetkisi
istiyor — bu yüzden hem TLS problemi yok, hem root gerekmiyor, hem de Linux ve
Windows'ta aynı şekilde çalışıyor.

## Neden iki adaptör

| Kaynak | Ne veriyor |
|---|---|
| `*.messages.log` | Uygulama katmanı: mesaj akışı, reject'ler, sequence takibi, order durumları |
| `*.event.log` | Session katmanı: disconnect **sebebi**, seq mismatch tespiti, heartbeat timeout, reconnect |

Mesaj logu akışın durduğunu söyler. Event logu **neden** durduğunu söyler.
İkisini `session_id` + zaman üzerinden korele etmek, bu projenin asıl işi.

## Mimari

```
  *.messages.log ──> MessageLogAdapter ─┐
                                        ├──> MPMC queue ──> pipeline ─┬──> SessionRegistry
  *.event.log    ──> EventLogAdapter  ──┘   (bounded)     (tek tüketici) ├──> MetricRegistry ──> /metrics
                                                                         └──> EventStore (SQLite)
```

Her adaptör kendi thread'inde tail yapıyor, event'leri paylaşılan bounded bir
Vyukov MPMC kuyruğuna basıyor. Tek tüketici thread'i state makinesini
güncelliyor, metrikleri artırıyor ve store'a batch yazıyor.

Kuyruk bilinçli olarak sınırlı: tüketici geride kalırsa büyümek yerine drop
ediyor ve drop'u sayıyor. Aynı makinede FIX engine koşuyorken sınırsız büyüyen
bir buffer kabul edilebilir değil.

### İki veri yolu

- **Prometheus** — sadece agregat. Label'lar `session`, `direction`, `msg_type`,
  `reason` ile sınırlı. ClOrdID, symbol, hesap ID'si asla metriğe girmiyor.
  Registry'de sabit bir seri limiti var; yanlış bir label scrape hedefini
  düşüremiyor.
- **SQLite** — tam sadakat. Her mesaj ve her session olayı, ham satırıyla
  birlikte. Delil burada; dashboard Prometheus'ta.

### Yeni kaynak eklemek

`ISourceAdapter` arayüzünü implement edip aynı `Event` struct'ını üretmek
yeterli. Şema pcap için hazır: `capture_ts_ns` ve `source` alanları duruyor,
log kaynaklarında `capture_ts_ns` null kalıyor. Bir pcap adaptörü eklendiğinde
aşağıdaki hiçbir katman değişmiyor.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/fixmon_selftest     # 60+ kontrol
```

Bağımlılık: sqlite3 ve pthread. Başka hiçbir şey yok.

### SQLite nereden geliyor

`cmake/ResolveSQLite3.cmake` dört yolu deniyor ve her durumda aynı
`SQLite::SQLite3` hedefini üretiyor, yani projenin geri kalanı kaynağı hiç
bilmiyor.

| `FIXMON_SQLITE_PROVIDER` | Davranış |
|---|---|
| `auto` (varsayılan) | Önce sistem, yoksa yerel dizin, o da yoksa indir |
| `system` | Sadece sistem; yoksa kurulum komutlarını gösterip hata ver |
| `local` | Sadece `FIXMON_SQLITE_SOURCE_DIR` (ağsız ortam) |
| `fetch` | Sistemde olsa bile indir |

Sistem önce deneniyor çünkü distro paketi yamalı ve normal kanaldan güvenlik
güncellemesi alıyor. Gömülü amalgamation pinlediğin sürümde donuyor, SQLite'ın
da CVE'leri oluyor. İndirme kolaylık yolu, tercih edilen yol değil.

```bash
# sistemde sqlite yoksa: indirip statik gömer
cmake -B build

# sürüm sabitleme (sqlite.org her sürümü yıl dizininde kalıcı tutuyor)
cmake -B build -DFIXMON_SQLITE_YEAR=2024 -DFIXMON_SQLITE_VERSION_ID=3460000 \
               -DFIXMON_SQLITE_SHA256=<hash>

# ağsız ortam
cmake -B build -DFIXMON_SQLITE_PROVIDER=local \
               -DFIXMON_SQLITE_SOURCE_DIR=/opt/sqlite-amalgamation
```

**Varsayılan indirme URL'i doğrulanmadı.** 404 alırsan `FIXMON_SQLITE_YEAR` ve
`FIXMON_SQLITE_VERSION_ID`'yi sqlite.org'daki gerçek bir sürüme ayarla, ya da
`FIXMON_SQLITE_URL`'i güvendiğin bir mirror'a çevir.

`FIXMON_SQLITE_SHA256` boş bırakılırsa indirme bütünlük kontrolü olmadan
yapılıyor ve CMake uyarı veriyor. Ürüne giden hiçbir build'de boş bırakma.

### Gerçek paket yöneticisi kullanmak

CMake paket yönetimi yapmıyor — `FetchContent` "kaynağı indir ve derle"
demek, bağımlılık çözümü, sürüm çakışması yönetimi, binary cache yok. Bunlar
gerekiyorsa vcpkg veya Conan kullan; ikisi de `find_package` üzerinden
çalıştığı için resolver onları `system` katmanında kendiliğinden buluyor,
CMake tarafında değişiklik gerekmiyor.

```bash
# vcpkg (vcpkg.json manifest'i repoda)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# conan (conanfile.txt repoda)
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
```

Windows'ta apt olmadığı için pratikte ya vcpkg ya da indirme yolu
kullanılacak; ikisi de test edildi.

## Çalıştırma

```bash
./build/fixmon fixmon.ini
```

```
event store: ./fixmon.db
session: FIX.4.4:BROKER1->VENUEX  hb=30s
metrics: http://0.0.0.0:9109/metrics
```

Endpoint'ler:

- `GET /metrics` — Prometheus text exposition
- `GET /sessions` — session durumu, JSON (debug için)
- `GET /healthz`

### Sahte log ile deneme

```bash
mkdir -p logs && touch logs/BROKER1-VENUEX.{messages,event}.log
./build/fixmon fixmon.ini &
python3 tools/gen_logs.py --dir ./logs --duration 30
curl -s localhost:9109/metrics | grep -v '^#'
```

Üreteç gerçek arıza senaryolarını enjekte ediyor: sequence gap + resend,
session-level reject, reddedilen ExecutionReport, business reject, heartbeat
timeout, sebepli disconnect + reconnect, seq-too-low, ve hiçbir kuralın
tanımadığı bir satır.

## Konfigürasyon

```ini
[global]
db_path          = ./fixmon.db
metrics_port     = 9109
queue_size       = 65536
batch_size       = 500
batch_flush_ms   = 200
poll_interval_ms = 100
from_beginning   = false
stale_after_multiple = 2

[session]
begin_string       = FIX.4.4
sender_comp_id     = BROKER1
target_comp_id     = VENUEX
message_log        = ./logs/BROKER1-VENUEX.messages.log
event_log          = ./logs/BROKER1-VENUEX.event.log
heartbeat_interval = 30
```

Session başına bir blok. Log yollarından biri boş bırakılabilir; o session için
bir adaptör az çalışır.

## Metrikler

**Uygulama katmanı**

| Metrik | Label'lar |
|---|---|
| `fixmon_messages_total` | session, direction, msg_type, msg_type_name |
| `fixmon_session_rejects_total` | session, reason, ref_tag |
| `fixmon_business_rejects_total` | session, reason |
| `fixmon_exec_rejects_total` | session, reason |

**Session katmanı**

| Metrik | Label'lar |
|---|---|
| `fixmon_session_events_total` | session, type |
| `fixmon_seq_gaps_total` | session, **src** |
| `fixmon_seq_gap_messages_total` | session, **src** |
| `fixmon_seq_too_low_total` | session |
| `fixmon_resend_requests_total` | session, **src** |
| `fixmon_heartbeat_timeouts_total` | session |
| `fixmon_disconnects_total` | session |
| `fixmon_reconnect_attempts_total` | session |
| `fixmon_logon_rejects_total` | session |

`src` label'ı önemli: aynı gap hem mesaj akışından türetiliyor hem event
logunda anlatılıyor. Toplamak yanlış sayım olurdu, o yüzden kaynağa göre
ayrıldı. İkisinin uyuşmaması ise başlı başına bir bulgu.

**Durum**

`fixmon_session_up`, `fixmon_session_state` (1 disconnected, 2 connecting,
3 logon_pending, 4 logged_on, 5 stale), `fixmon_last_message_age_seconds`,
`fixmon_next_expected_seq_num`, `fixmon_last_outgoing_seq_num`

**Collector'ın kendisi**

`fixmon_events_dropped_total`, `fixmon_queue_depth`,
`fixmon_store_rows_written_total`, `fixmon_store_write_errors_total`,
`fixmon_metric_series`

## Event store

WAL + `synchronous=NORMAL`, batch'ler açık transaction içinde. Tek tek insert
bu iş yükünü kaldırmaz.

İki tablo:

- `events` — her mesaj ve her session olayı, `raw` sütununda ham satırla.
  İndeksler `(session_id, ts_ns)` sorgu şekline göre.
- `session_snapshots` — 30 saniyede bir session durumu, "14:32'de bu session
  neye benziyordu" sorusunu event'leri baştan oynatmadan cevaplamak için.

Incident timeline sorgusu:

```sql
SELECT datetime(ts_ns/1000000000,'unixepoch') AS t,
       source,
       COALESCE(msg_type_name, session_event) AS what,
       text
FROM events
WHERE session_id = 'FIX.4.4:BROKER1->VENUEX'
  AND ts_ns BETWEEN ? AND ?
  AND (session_event IN ('disconnected','heartbeat_timeout',
                         'seq_num_too_high','seq_num_too_low')
       OR msg_type IN ('3','j')
       OR (msg_type='8' AND ord_status='8'))
ORDER BY ts_ns;
```

Çıktısı şuna benziyor:

```
t                    source       what                   text
2026-09-04 20:29:10  event_log    seq_num_too_high       MsgSeqNum too high, expecting 67 but received 73
2026-09-04 20:29:14  message_log  Reject                 Required tag missing
2026-09-04 20:29:17  message_log  ExecutionReport        Instrument not tradable at this time
2026-09-04 20:29:21  event_log    heartbeat_timeout      Test Request timed out
2026-09-04 20:29:23  event_log    disconnected           Socket exception, connection reset by peer
```

İki kaynağın tek zaman ekseninde birleşmesi tam olarak bu.

## Grafana ve alert'ler

`grafana/fixmon-sessions.json` — session durumu, sessizlik süresi, mesaj
oranları, reject kırılımı, sequence sağlığı, collector sağlığı.

`grafana/alerts.yml` — session down, stale, flapping, seq-too-low, yükselen
reject oranı, heartbeat timeout, artı collector'ın kendi sağlığı. Eşikler
başlangıç değeri; her karşı tarafın davranışına göre ayarlanmalı.

## Bilinen sınırlar

Log kaynağının yapısal olarak göremedikleri:

- TCP seviyesi sağlık (retransmit, zero-window, RTT)
- Wire latency — sadece engine'in kendi timestamp'i var, `capture_ts_ns` boş
- Engine'e hiç ulaşmamış olaylar: TLS handshake hatası, firewall drop, venue'nun
  SYN'e cevap vermemesi
- Log seviyesi kısılmış veya kapatılmışsa hiçbir şey

Bunların bir kısmı ucuz kontrollerle kapatılabilir (TCP bağlantı probe'u,
sertifika süresi kontrolü, socket durumu okuma); geri kalanı pcap adaptörünü
gerektiriyor.

Parse edilemeyen event log satırları atılmıyor — `unparsed` olarak ham haliyle
saklanıyor ve `fixmon_session_events_total{type="unparsed"}` altında sayılıyor.
Bu metriğin yükselmesi, engine sürümünün veya vendor'ün yeni bir ifade
kullanmaya başladığının işareti.

Parse kuralları şu an `src/event_log_adapter.cpp` içinde. Yapıları kasten
tablo şeklinde: dışarı alınması bu adaptörün mantığını değiştirmeyecek.
