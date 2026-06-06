# BIL304-OTA-Project
# BIL304 - OTA Firmware Update Project

## Proje Amacı

Bu proje, Contiki-NG işletim sistemi üzerinde çalışan Z1 düğümleri arasında OTA (Over-The-Air) firmware güncelleme mekanizmasının gerçekleştirilmesini amaçlamaktadır. Amaç, bir firmware imajının kablosuz ağ üzerinden güvenilir biçimde aktarılması, alıcı düğüm tarafından kalıcı depolama alanına kaydedilmesi ve bütünlüğünün doğrulanmasıdır.

---

# Sistem Mimarisi

Projede üç adet düğüm bulunmaktadır:

| Node ID | Görev                  |
| ------- | ---------------------- |
| 1       | Alıcı (UDP Server)     |
| 2       | Gönderici (UDP Client) |
| 3       | Ara Düğüm (Forwarder)  |

Node 2 tarafından gönderilen firmware blokları RPL yönlendirmesi kullanılarak Node 3 üzerinden Node 1'e ulaştırılmaktadır.

---

# OTA Paket Yapısı

Firmware verisi 64 baytlık bloklara bölünerek gönderilmektedir.

```c
struct ota_packet {
  uint16_t block_number;
  uint16_t total_blocks;
  uint16_t data_len;
  uint16_t checksum;
  uint32_t total_size;
  uint32_t image_crc32;
  uint8_t flags;
  uint8_t payload[64];
};
```

Paket alanlarının görevleri:

* block_number : Blok numarası
* total_blocks : Toplam blok sayısı
* data_len : Paket içindeki gerçek veri miktarı
* checksum : Paket doğrulama değeri
* total_size : Firmware toplam boyutu
* image_crc32 : Firmware CRC32 değeri
* flags : Son blok bilgisi
* payload : Firmware verisi

---

# Firmware Parçalama

Firmware imajı sabit boyutlu bloklara ayrılmıştır.

```c
#define CHUNK_SIZE 64
```

Her blok ayrı UDP paketi olarak gönderilmektedir.

Bu yaklaşım sayesinde büyük boyutlu firmware dosyalarının aktarılması mümkün hale gelmektedir.

---

# Tasarım Kararları

Bu projede OTA aktarım mekanizması tasarlanırken gömülü sistemlerin bellek ve haberleşme kısıtları dikkate alınmıştır.

## Paket Boyutu Seçimi

Firmware verisi 64 baytlık bloklara bölünerek gönderilmiştir.

```c
#define CHUNK_SIZE 64
```

64 baytlık paket boyutu seçilmesinin temel sebepleri:

* IEEE 802.15.4 tabanlı ağlarda paket boyutlarının sınırlı olması,
* UDP ve IPv6 başlıklarının veri çerçevesi içerisinde yer kaplaması,
* Büyük paketlerde hata oluşma ihtimalinin artması,
* Küçük paketlerde yeniden gönderim maliyetinin daha düşük olmasıdır.

Bu nedenle firmware imajı sabit boyutlu 64 baytlık bloklara ayrılmıştır.

---

# Güvenilir Aktarım Stratejisi

Projede Stop-and-Wait ARQ yöntemi kullanılmıştır.

Çalışma prensibi:

1. Gönderici bir blok gönderir.
2. ACK bekler.
3. ACK gelirse sonraki bloğa geçer.
4. ACK gelmezse aynı blok yeniden gönderilir.

Örnek kod:

```c
if(waiting_for_ack) {
  LOG_WARN("Zaman asimi! Blok yeniden gonderiliyor.\n");
}
```

Bu yöntem paket kayıplarına karşı koruma sağlamaktadır.

---

# Durum Yönetimi

Sistemde hangi bloğun gönderildiği ve hangi bloğun onaylandığı takip edilmektedir.

Gönderici tarafında:

```c
static uint16_t current_block_number = 0;
static bool waiting_for_ack = false;
```

değişkenleri kullanılmaktadır.

* current_block_number gönderilecek mevcut bloğu,
* waiting_for_ack ise ACK beklenip beklenmediğini göstermektedir.

ACK alındığında:

```c
current_block_number++;
waiting_for_ack = false;
```

işlemleri yapılmaktadır.

Bu yapı sayesinde bloklar sıralı biçimde aktarılmaktadır.

---

# Paket Doğrulama (Checksum)

Her blok gönderilmeden önce checksum değeri hesaplanmaktadır.

```c
pkt.checksum = calculate_checksum(pkt.payload, n);
```

Alıcı tarafta tekrar hesaplanarak karşılaştırılır.

```c
if(computed_checksum != pkt->checksum) {
  LOG_ERR("Checksum hatasi");
}
```

Bu sayede bozuk paketler tespit edilmektedir.

---

# Alınan Güvenilirlik Önlemleri

## 1. Paket Checksum Doğrulaması

Her paket için checksum hesaplanmaktadır.

## 2. ACK Mekanizması

Alıcı düğüm başarılı şekilde aldığı her blok için ACK göndermektedir.

```c
simple_udp_sendto(&udp_conn,
                  &ack_block,
                  sizeof(ack_block),
                  sender_addr);
```

## 3. Stop-and-Wait ARQ

Her blok gönderildikten sonra ACK beklenmektedir.

ACK alınamazsa aynı blok tekrar gönderilmektedir.

## 4. CRC32 Tüm-İmaj Doğrulaması

Aktarım tamamlandıktan sonra tüm firmware için CRC32 doğrulaması yapılmaktadır.

## 5. Kalıcı Depolama

Firmware parçaları Coffee File System (CFS) üzerine kaydedilmektedir.

## 6. OTA Metadata

Firmware sürümü, CRC değeri ve güncelleme durumu metadata yapısında tutulmaktadır.

---

# CRC32 Tüm-İmaj Doğrulaması

Aktarım sonunda tüm firmware için CRC32 hesaplanmaktadır.

Kullanılan fonksiyon:

```c
ota_crc32_buffer(...)
```

Aktarım sonunda:

```text
Beklenen CRC32
Hesaplanan CRC32
```

değerleri karşılaştırılmaktadır.

Başarılı doğrulama durumunda:

```text
Tum imaj dogrulamasi basarili.
```

mesajı üretilmektedir.

---

# Kalıcı Depolama (CFS)

Alıcı düğüm gelen blokları Coffee File System (CFS) üzerine yazmaktadır.

```c
int fd = cfs_open("received_fw.bin", CFS_WRITE | CFS_READ);

cfs_seek(fd, pkt->block_number * CHUNK_SIZE, CFS_SEEK_SET);

cfs_write(fd, pkt->payload, pkt->data_len);
```

Bu yöntem sayesinde firmware parçaları doğru sırayla depolanmaktadır.

---

# OTA Metadata Yapısı

Projede OTA güncelleme durumunu takip etmek amacıyla metadata mekanizması geliştirilmiştir.

Metadata içerisinde:

* Aktif slot
* Aday slot
* Firmware versiyonu
* Firmware boyutu
* Firmware CRC değeri
* Güncelleme durumu

saklanmaktadır.

Kullanılan durumlar:

```c
OTA_IMAGE_STATE_EMPTY
OTA_IMAGE_STATE_VERIFIED
OTA_IMAGE_STATE_PENDING
OTA_IMAGE_STATE_CONFIRMED
```

---

# Hash ve Bütünlük Doğrulaması

Bu projede SHA-256, SHA-1 veya MD5 gibi kriptografik hash algoritmaları kullanılmamıştır.

Bunun yerine:

* Paket doğrulaması için 16 bit Checksum,
* Dosya doğrulaması için CRC32

kullanılmıştır.

CRC32 algoritması, kablosuz aktarım sırasında oluşabilecek veri bozulmalarını tespit etmek amacıyla tercih edilmiştir.

Şartnamede belirtilen:

> Hash, CRC32 veya benzeri bir bütünlük mekanizması

gereksinimi bu yöntem ile karşılanmıştır.

---

# Gerçeklenen İsterler

| İster               | Durum |
| ------------------- | ----- |
| Firmware parçalama  | ✓     |
| Blok numaralandırma | ✓     |
| Paket checksum      | ✓     |
| CRC32 doğrulama     | ✓     |
| Yeniden gönderim    | ✓     |
| Stop-and-Wait       | ✓     |
| Kalıcı depolama     | ✓     |
| Dosya birleştirme   | ✓     |
| Durum yönetimi      | ✓     |
| Başarı mesajı       | ✓     |

---

# Simülasyon Sonuçları

Örnek çıktı:

```text
Blok 127 alindi ve diske kaydedildi.

ACK Alindi: Blok 127 onaylandi.

Son blok alindi.

Toplam blok: 128
Toplam boyut: 8192 bayt

Beklenen CRC32: 0x66b06bea
Hesaplanan CRC32: 0x66b06bea

Tum imaj dogrulamasi basarili.

Yuklenmeye hazir yeni firmware alimi tamamlandi.
```

---

# Kullanılan Teknolojiler

* Contiki-NG
* Cooja Simulator
* RPL Routing
* UDP
* Coffee File System (CFS)
* CRC32
* Checksum
* MSP430 Z1 Platformu

---

# Video Sunumu

YouTube video linki: https://youtu.be/cbHrY9CZxcA


