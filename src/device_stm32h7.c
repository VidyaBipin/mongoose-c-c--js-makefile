#include "device.h"
#include "log.h"

#if MG_DEVICE == MG_DEVICE_STM32H7

#define FLASH_BASE1 0x52002000  // Base address for bank1
#define FLASH_BASE2 0x52002100  // Base address for bank2
#define FLASH_KEYR 0x04         // See RM0433 4.9.2
#define FLASH_OPTKEYR 0x08
#define FLASH_OPTCR 0x18
#define FLASH_SR 0x10
#define FLASH_CR 0x0c
#define FLASH_CCR 0x14
#define FLASH_OPTSR_CUR 0x1c
#define FLASH_OPTSR_PRG 0x20
#define FLASH_SIZE_REG 0x1FF1E880

void *MG_IRAM mg_flash_start(void) {
  return (void *) 0x08000000;
}
size_t MG_IRAM mg_flash_size(void) {
  return MG_REG(FLASH_SIZE_REG) * 1024;  // convert from KB to bytes
}
size_t MG_IRAM mg_flash_sector_size(void) {
  return 128 * 1024;  // 128k
}
size_t MG_IRAM mg_flash_write_align(void) {
  return 32;  // 256 bit
}
int MG_IRAM mg_flash_bank(void) {
#if MG_DEVICE_DUAL_BANK
  return MG_REG(FLASH_BASE1 + FLASH_OPTCR) & MG_BIT(31) ? 2 : 1;
#else
  return 1;
#endif
}

static void MG_IRAM flash_unlock(void) {
  static bool unlocked = false;
#if MG_DEVICE_DUAL_BANK == 0
  if (mg_ota_status(MG_FIRMWARE_CURRENT) == MG_OTA_FIRST_BOOT) unlocked = true;
#endif
  if (unlocked == false) {
    MG_REG(FLASH_BASE1 + FLASH_KEYR) = 0x45670123;
    MG_REG(FLASH_BASE1 + FLASH_KEYR) = 0xcdef89ab;
#if MG_DEVICE_DUAL_BANK
    MG_REG(FLASH_BASE2 + FLASH_KEYR) = 0x45670123;
    MG_REG(FLASH_BASE2 + FLASH_KEYR) = 0xcdef89ab;
#endif
    MG_REG(FLASH_BASE1 + FLASH_OPTKEYR) = 0x08192a3b;  // opt reg is "shared"
    MG_REG(FLASH_BASE1 + FLASH_OPTKEYR) = 0x4c5d6e7f;  // thus unlock once
    unlocked = true;
  }
}

static bool MG_IRAM flash_page_start(volatile uint32_t *dst) {
  char *base = (char *) mg_flash_start(), *end = base + mg_flash_size();
  volatile char *p = (char *) dst;
  return p >= base && p < end && ((p - base) % mg_flash_sector_size()) == 0;
}

static bool MG_IRAM flash_is_err(uint32_t bank) {
  return MG_REG(bank + FLASH_SR) & ((MG_BIT(11) - 1) << 17);  // RM0433 4.9.5
}

static void MG_IRAM flash_wait(uint32_t bank) {
  while (MG_REG(bank + FLASH_SR) & (MG_BIT(0) | MG_BIT(2))) (void) 0;
}

static void MG_IRAM flash_clear_err(uint32_t bank) {
  flash_wait(bank);                                      // Wait until ready
  MG_REG(bank + FLASH_CCR) = ((MG_BIT(11) - 1) << 16U);  // Clear all errors
}

#if MG_DEVICE_DUAL_BANK
static bool flash_bank_is_swapped(uint32_t bank) {
  return MG_REG(bank + FLASH_OPTCR) & MG_BIT(31);  // RM0433 4.9.7
}
#endif

// Figure out flash bank based on the address
static uint32_t MG_IRAM flash_bank(void *addr) {
#if MG_DEVICE_DUAL_BANK
  size_t ofs = (char *) addr - (char *) mg_flash_start();
  return ofs < mg_flash_size() / 2 ? FLASH_BASE1 : FLASH_BASE2;
#else
  (void) addr;
  return FLASH_BASE1;
#endif
}

bool MG_IRAM mg_flash_erase(void *addr) {
  bool ok = false;
  if (flash_page_start(addr) == false) {
    MG_ERROR(("%p is not on a sector boundary", addr));
  } else {
    uintptr_t diff = (char *) addr - (char *) mg_flash_start();
    uint32_t sector = diff / mg_flash_sector_size();
    uint32_t bank = flash_bank(addr);

    flash_unlock();
    if (sector > 7) sector -= 8;
    // MG_INFO(("Erasing @ %p, sector %lu, bank %#x", addr, sector, bank));

    flash_clear_err(bank);
    MG_REG(bank + FLASH_CR) |= (sector & 7U) << 8U;  // Sector to erase
    MG_REG(bank + FLASH_CR) |= MG_BIT(2);            // Sector erase bit
    MG_REG(bank + FLASH_CR) |= MG_BIT(7);            // Start erasing
    ok = !flash_is_err(bank);
    MG_DEBUG(("Erase sector %lu @ %p %s. CR %#lx SR %#lx", sector, addr,
              ok ? "ok" : "fail", MG_REG(bank + FLASH_CR),
              MG_REG(bank + FLASH_SR)));
    // mg_hexdump(addr, 32);
  }
  return ok;
}

bool mg_flash_swap_bank() {
#if MG_DEVICE_DUAL_BANK
  uint32_t bank = FLASH_BASE1;
  uint32_t desired = flash_bank_is_swapped(bank) ? 0 : MG_BIT(31);
  flash_unlock();
  flash_clear_err(bank);
  // printf("OPTSR_PRG 1 %#lx\n", FLASH->OPTSR_PRG);
  MG_SET_BITS(MG_REG(bank + FLASH_OPTSR_PRG), MG_BIT(31), desired);
  // printf("OPTSR_PRG 2 %#lx\n", FLASH->OPTSR_PRG);
  MG_REG(bank + FLASH_OPTCR) |= MG_BIT(1);  // OPTSTART
  while ((MG_REG(bank + FLASH_OPTSR_CUR) & MG_BIT(31)) != desired) (void) 0;
#endif
  return true;
}

bool MG_IRAM mg_flash_write(void *addr, const void *buf, size_t len) {
  if ((len % mg_flash_write_align()) != 0) {
    MG_ERROR(("%lu is not aligned to %lu", len, mg_flash_write_align()));
    return false;
  }
  uint32_t bank = flash_bank(addr);
  uint32_t *dst = (uint32_t *) addr;
  uint32_t *src = (uint32_t *) buf;
  uint32_t *end = (uint32_t *) ((char *) buf + len);
  bool ok = true;
  flash_unlock();
  flash_clear_err(bank);
  MG_ARM_DISABLE_IRQ();
  MG_REG(bank + FLASH_CR) = MG_BIT(1);  // Set programming flag
  // MG_INFO(("Writing flash @ %p, %lu bytes", addr, len));
  while (ok && src < end) {
    if (flash_page_start(dst) && mg_flash_erase(dst) == false) break;
    *(volatile uint32_t *) dst++ = *src++;
    flash_wait(bank);
    if (flash_is_err(bank)) ok = false;
  }
  MG_DEBUG(("Flash write %lu bytes @ %p: %s. CR %#lx SR %#lx", len, dst,
            ok ? "ok" : "fail", MG_REG(bank + FLASH_CR),
            MG_REG(bank + FLASH_SR)));
  // mg_hexdump(addr, len > 32 ? 32 : len);
  MG_REG(bank + FLASH_CR) &= ~MG_BIT(1);  // Clear programming flag
  MG_ARM_ENABLE_IRQ();
  return ok;
}

void mg_device_reset(void) {
  // SCB->AIRCR = ((0x5fa << SCB_AIRCR_VECTKEY_Pos)|SCB_AIRCR_SYSRESETREQ_Msk);
  *(volatile unsigned long *) 0xe000ed0c = 0x5fa0004;
}
#endif