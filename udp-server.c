/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */
#include "ota-metadata.h"
#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"

#include "sys/log.h"
#include "cfs/cfs.h" // Dosya işlemleri için gerekli
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define CHUNK_SIZE 64 // Her seferde gönderilecek bayt miktarı
#define OTA_FLAG_LAST_BLOCK 1

struct ota_packet {
  uint16_t block_number;
  uint16_t total_blocks;
  uint16_t data_len;
  uint16_t checksum;
  uint32_t total_size;
  uint32_t image_crc32;
  uint8_t flags;
  uint8_t payload[CHUNK_SIZE];
};


uint16_t calculate_checksum(const uint8_t *data, uint16_t length) {
  uint32_t sum = 0;
  for(uint16_t i = 0; i < length; i++) {
    sum += data[i];
  }
  while(sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return (uint16_t)(~sum);
}

static struct simple_udp_connection udp_conn;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;
  // Gelen veriyi ota_packet yapısına dönüştür
  struct ota_packet *pkt = (struct ota_packet *)data;


  uint16_t computed_checksum = calculate_checksum(pkt->payload, pkt->data_len);
  if(computed_checksum != pkt->checksum) {
    LOG_ERR("Blok %u REDDEDİLDİ! Checksum hatası.\n", pkt->block_number);
    return; 
  }
  // Dosyayı yazma ve ekleme (append) modunda aç
  int fd = cfs_open("received_fw.bin", CFS_WRITE | CFS_READ);
  
  if(fd >= 0) {
    // Veriyi kalıcı depolama alanına yaz
    
    cfs_seek(fd, pkt->block_number * CHUNK_SIZE, CFS_SEEK_SET);
    cfs_write(fd, pkt->payload, pkt->data_len);
    cfs_close(fd);
    
    LOG_INFO("Blok %u alindi ve diske kaydedildi.\n", pkt->block_number);

    // Göndericiye sadece onayladığımız blok numarasını geri gönderiyoruz
    uint16_t ack_block = pkt->block_number;
    simple_udp_sendto(&udp_conn, &ack_block, sizeof(ack_block), sender_addr);
    
    // PDF'de istenen başarı mesajı kontrolü (yaklaşık dosya boyutu bittiğinde)
    if(pkt->flags & OTA_FLAG_LAST_BLOCK) {
   int rfd;
   uint8_t buf[CHUNK_SIZE];
   uint32_t calc_crc = 0;
   uint32_t total_read = 0;
   int n;

   rfd = cfs_open("received_fw.bin", CFS_READ);

   if(rfd >= 0) {
     while((n = cfs_read(rfd, buf, sizeof(buf))) > 0) {
       calc_crc = ota_crc32_update_stream(calc_crc, buf, n);
       total_read += n;
     }
     cfs_close(rfd);
   }

   LOG_INFO("Son blok alindi. Toplam blok: %u, toplam boyut: %lu bayt, beklenen CRC32: 0x%08lx\n",
            pkt->total_blocks,
            (unsigned long)pkt->total_size,
            (unsigned long)pkt->image_crc32);

   LOG_INFO("Alinan boyut: %lu bayt, hesaplanan CRC32: 0x%08lx\n",
            (unsigned long)total_read,
            (unsigned long)calc_crc);

   if(total_read == pkt->total_size) {
     LOG_INFO("Tum imaj boyut dogrulamasi basarili.\n");
   }

   LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
}
  }else {
    LOG_ERR("HATA: Kalıcı depolama dosyası açılamadı!\n");
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  /* Initialize DAG root */
  NETSTACK_ROUTING.root_start();

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
