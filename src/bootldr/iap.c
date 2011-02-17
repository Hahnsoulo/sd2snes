#include <arm/NXP/LPC17xx/LPC17xx.h>
#include <string.h>
#include "bits.h"
#include "iap.h"
#include "config.h"
#include "uart.h"
#include "fileops.h"
#include "crc32.h"

uint32_t iap_cmd[5];
uint32_t iap_res[5];
uint32_t flash_sig[4];

IAP iap_entry = (IAP) IAP_LOCATION;

uint32_t calc_flash_crc(uint32_t start, uint32_t len) {
  printf("calc_flash_crc(%08lx, %08lx) {\n", start, len);
  uint32_t end = start + len;
  if(end > 0x20000) {
    len = 0x1ffff - start;
    end = 0x20000;
  }
  uint32_t crc = 0xffffffff;
  uint32_t s = start;
  while(s < end) {
    crc = crc32_update(crc, *(const unsigned char*)(s));
    s++;
  }
  crc ^= 0xffffffff;
  printf("  crc generated. result=%08lx\n", crc);
  printf("} //calc_flash_crc\n");
  return crc;
}

void test_iap() {
  iap_cmd[0]=54;
  iap_entry(iap_cmd, iap_res);
  printf("Part ID=%08lx\n", iap_res[1]);
}

void print_header(sd2snes_fw_header *header) {
  printf("  magic = %08lx\n  version = %08lx\n  size = %08lx\n  crc = %08lx\n  ~crc = %08lx\n",
         header->magic, header->version, header->size,
         header->crc, header->crcc);
}

int check_header(sd2snes_fw_header *header, uint32_t crc) {
  if((header->magic != FW_MAGIC)
     || (header->size < 0x200)
     || (header->size > (0x1ffff - FW_START))
     || ((header->crc ^ header->crcc) != 0xffffffff)) {
    return ERR_FLASHHD;
  }
  if(header->crc != crc) {
    return ERR_FLASHCRC;
  }
  return ERR_OK;
}

FLASH_RES check_flash() {
  sd2snes_fw_header *fw_header = (sd2snes_fw_header*) FW_START;
  uint32_t flash_addr = FW_START;
  if(flash_addr != FW_START) {
    printf("address sanity check failed. expected 0x%08lx, got 0x%08lx.\nSomething is terribly wrong.\nBailing out to avoid bootldr self-corruption.\n", FW_START, flash_addr);
    return ERR_HW;
  }
  printf("Current flash contents:\n");
  print_header(fw_header);
  uint32_t crc = calc_flash_crc(flash_addr + 0x100, (fw_header->size & 0x1ffff));
  return check_header(fw_header, crc);
}

IAP_RES iap_wrap(uint32_t *iap_cmd, uint32_t *iap_res) {
  NVIC_DisableIRQ(RIT_IRQn);
  NVIC_DisableIRQ(UART_IRQ);
  iap_entry(iap_cmd, iap_res);
  NVIC_EnableIRQ(RIT_IRQn);
  NVIC_EnableIRQ(UART_IRQ);
  return iap_res[0];
}

IAP_RES iap_prepare_for_write(uint32_t start, uint32_t end) {
  if(start < (FW_START / 0x1000)) return INVALID_SECTOR;
  iap_cmd[0] = 50;
  iap_cmd[1] = start;
  iap_cmd[2] = end;
  iap_wrap(iap_cmd, iap_res);
  return iap_res[0];
}

IAP_RES iap_erase(uint32_t start, uint32_t end) {
  if(start < (FW_START / 0x1000)) return INVALID_SECTOR;
  iap_cmd[0] = 52;
  iap_cmd[1] = start;
  iap_cmd[2] = end;
  iap_cmd[3] = CONFIG_CPU_FREQUENCY / 1000L;
  iap_wrap(iap_cmd, iap_res);
  return iap_res[0];
}

IAP_RES iap_ram2flash(uint32_t tgt, uint8_t *src, int num) {
  iap_cmd[0] = 51;
  iap_cmd[1] = tgt;
  iap_cmd[2] = (uint32_t)src;
  iap_cmd[3] = num;
  iap_cmd[4] = CONFIG_CPU_FREQUENCY / 1000L;
  iap_wrap(iap_cmd, iap_res);
  return iap_res[0];
}

FLASH_RES flash_file(uint8_t *filename) {
  sd2snes_fw_header *fw_header = (sd2snes_fw_header*) FW_START;
  uint32_t flash_addr = FW_START;
  uint32_t file_crc = 0xffffffff;
  uint16_t count;
  sd2snes_fw_header file_header;
  UINT bytes_read;
  if(flash_addr != FW_START) {
    printf("address sanity check failed. expected 0x%08lx, got 0x%08lx.\nSomething is terribly wrong.\nBailing out to avoid bootldr self-corruption.\n", FW_START, flash_addr);
    return ERR_HW;
  }
  file_open(filename, FA_READ);
  if(file_res) {
    printf("file_open: error %d\n", file_res);
    return ERR_FS;
  }
  printf("firmware image found. file size: %ld\n", file_handle.fsize);
  printf("reading header...\n");
  f_read(&file_handle, &file_header, 32, &bytes_read);
  print_header(&file_header);
  if(check_flash() || file_header.version != fw_header->version || file_header.version == FW_MAGIC || fw_header->version == FW_MAGIC) {
    f_lseek(&file_handle, 0x100);
    for(;;) {
      bytes_read = file_read();
      if(file_res || !bytes_read) break;
      for(count = 0; count < bytes_read; count++) {
        file_crc = crc32_update(file_crc, file_buf[count]);
      }
    }
    file_crc ^= 0xffffffff;
    printf("file crc=%08lx\n", file_crc);
    if(check_header(&file_header, file_header.crc) != ERR_OK) {
      printf("Invalid firmware file (header corrupted).\n");
      return ERR_FILEHD;
    }
    if(file_header.crc != file_crc) {
      printf("Firmware file checksum error.\n");
      return ERR_FILECHK;
    }

    uint32_t res;

    printf("erasing flash...\n");
    if((res = iap_prepare_for_write(FW_START / 0x1000, FLASH_SECTORS)) != CMD_SUCCESS) {
      printf("error %ld while preparing for erase\n", res);
      return ERR_FLASHPREP;
    };
    if((res = iap_erase(FW_START / 0x1000, FLASH_SECTORS)) != CMD_SUCCESS) {
      printf("error %ld while erasing\n", res);
      return ERR_FLASHERASE;
    }
    printf("writing... @%08lx\n", flash_addr);
    f_lseek(&file_handle, 0);
    uint8_t current_sec;

    for(flash_addr = FW_START; flash_addr < 0x00020000; flash_addr += 0x200) {
      bytes_read = file_read();
      if(file_res || !bytes_read) break;
      current_sec = flash_addr & 0x10000 ? (16 + ((flash_addr >> 15) & 1))
                                         : (flash_addr >> 12);
      printf("current_sec=%d flash_addr=%08lx\n", current_sec, flash_addr);
      if(current_sec < (FW_START / 0x1000)) return ERR_FLASH;
      if((res = iap_prepare_for_write(current_sec, current_sec)) != CMD_SUCCESS) {
        printf("error %ld while preparing sector %d for write\n", res, current_sec);
      }
      if((res = iap_ram2flash(flash_addr, file_buf, 512)) != CMD_SUCCESS) {
        printf("error %ld while writing to address %08lx (sector %d)\n", res, flash_addr, current_sec);
      }
    }
  } else {
    printf("flash content is ok, no version mismatch, no forced upgrade. No need to flash\n");
  }
  return ERR_OK;
}
