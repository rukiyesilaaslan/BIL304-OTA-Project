#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "ota-metadata.h"
#include "firmware_data.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "sys/node-id.h"
#include "sys/log.h"
#include "cfs/cfs.h" // Dosya işlemleri için gerekli
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678

#define SEND_INTERVAL		  (10 * CLOCK_SECOND)
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

static uint16_t current_block_number = 0; // Hangi blokta kaldığımızı izleyen durum 
static bool waiting_for_ack = false;       
static bool transfer_done = false;
static struct simple_udp_connection udp_conn;
static uint32_t rx_count = 0;
static ota_boot_metadata_t boot_metadata = {
  .magic = OTA_IMAGE_MAGIC,
  .active_slot = OTA_SLOT_A,
  .candidate_slot = OTA_SLOT_NONE,
  .state_a = OTA_IMAGE_STATE_CONFIRMED,
  .state_b = OTA_IMAGE_STATE_EMPTY,

};

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
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
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(datalen == sizeof(uint16_t)) {
    uint16_t acked_block = *(uint16_t *)data;
    if(acked_block == current_block_number && waiting_for_ack) {
      LOG_INFO("ACK Alindi: Blok %u onaylandi.\n", acked_block);
      current_block_number++; 
      waiting_for_ack = false; // Bekleme durumunu kaldır
      rx_count++;
    }
  }

  static uint32_t fake_version = 2;
  uint32_t fake_image_crc = 0;
  if(ota_metadata_mark_verified(&boot_metadata, OTA_SLOT_B,
                                fake_version, datalen, fake_image_crc) &&
     ota_metadata_stage_verified_image(&boot_metadata, OTA_SLOT_B)) {
    LOG_INFO("OTA metadata updated: slot B staged for activation\n");
  }
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  uip_ipaddr_t dest_ipaddr;
  static uint32_t missed_tx_count;

  PROCESS_BEGIN();

  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

// Bu blok 3 numarali cihazda calismaz. 2-den-1-e gonderim icin yapildi. 3 numarali
// cihaz komsuluk gorevi yapar. İletime yardim eder.

      if(node_id == 2) {
        if(transfer_done) {
         etimer_set(&periodic_timer, SEND_INTERVAL);
         continue;
        }
        static struct ota_packet pkt;
        uint32_t offset = current_block_number * CHUNK_SIZE;

        if(offset < new_firmware_z1_len) {
          uint16_t n = CHUNK_SIZE;

          if(offset + CHUNK_SIZE > new_firmware_z1_len) {
            n = new_firmware_z1_len - offset;
          }

          pkt.block_number = current_block_number;
	  pkt.total_blocks = (new_firmware_z1_len + CHUNK_SIZE - 1) / CHUNK_SIZE;
	  pkt.data_len = n;
	  pkt.total_size = new_firmware_z1_len;
	  pkt.image_crc32 = ota_crc32_buffer(new_firmware_z1, new_firmware_z1_len);
	  pkt.flags = 0;

	  if(current_block_number == pkt.total_blocks - 1) {
 	    pkt.flags |= OTA_FLAG_LAST_BLOCK;
	  }

	  memcpy(pkt.payload, &new_firmware_z1[offset], n);
	  pkt.checksum = calculate_checksum(pkt.payload, n);
          
          
          if(waiting_for_ack) {
            LOG_WARN("Zaman asimi! Blok %u YENIDEN GONDERILIYOR...\n", pkt.block_number);
          } else {
            LOG_INFO("Blok %u gonderiliyor (%u bayt)...\n", pkt.block_number, n);
          }

          simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dest_ipaddr);
          waiting_for_ack = true;

        } else {
          LOG_INFO("Dosya sonuna ulasildi, aktarim tamamlandi.\n");
          transfer_done = true;
        }
      }
    }else {
      LOG_INFO("Not reachable yet\n");
      if(current_block_number > 0) {
        missed_tx_count++;
      }
    }

    // Zaman aşımı ve periyot kontrolü (Stop-and-wait için periyodu kısa tutabilirsin)
    etimer_set(&periodic_timer, SEND_INTERVAL);
  }
  
  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
