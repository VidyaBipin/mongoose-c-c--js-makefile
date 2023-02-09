#pragma once

#include "arch.h"
#include "net.h"

struct mg_tcpip_if;  // MIP network interface

struct mg_tcpip_driver {
  bool (*init)(struct mg_tcpip_if *);                         // Init driver
  size_t (*tx)(const void *, size_t, struct mg_tcpip_if *);   // Transmit frame
  size_t (*rx)(void *buf, size_t len, struct mg_tcpip_if *);  // Receive frame
  bool (*up)(struct mg_tcpip_if *);                           // Up/down status
};

// Receive queue - single producer, single consumer queue.  Interrupt-based
// drivers copy received frames to the queue in interrupt context.
// mg_tcpip_poll() function runs in event loop context, reads from the queue
struct queue {
  uint8_t *buf;
  size_t len;
  volatile size_t tail, head;
};

// Network interface
struct mg_tcpip_if {
  uint8_t mac[6];                  // MAC address. Must be set to a valid MAC
  uint32_t ip, mask, gw;           // IP address, mask, default gateway
  struct mg_str rx;                // Output (TX) buffer
  struct mg_str tx;                // Input (RX) buffer
  bool enable_dhcp_client;         // Enable DCHP client
  bool enable_dhcp_server;         // Enable DCHP server
  struct mg_tcpip_driver *driver;  // Low level driver
  void *driver_data;               // Driver-specific data
  struct mg_mgr *mgr;              // Mongoose event manager
  struct queue queue;              // Set queue.len for interrupt based drivers

  // Internal state, user can use it but should not change it
  uint8_t gwmac[6];         // Router's MAC
  uint64_t now;             // Current time
  uint64_t timer_1000ms;    // 1000 ms timer: for DHCP and link state
  uint64_t lease_expire;    // Lease expiration time
  uint16_t eport;           // Next ephemeral port
  volatile uint32_t ndrop;  // Number of received, but dropped frames
  volatile uint32_t nrecv;  // Number of received frames
  volatile uint32_t nsent;  // Number of transmitted frames
  volatile uint32_t nerr;   // Number of driver errors
  uint8_t state;            // Current state
#define MIP_STATE_DOWN 0    // Interface is down
#define MIP_STATE_UP 1      // Interface is up
#define MIP_STATE_READY 2   // Interface is up and has IP
};

void mg_tcpip_init(struct mg_mgr *, struct mg_tcpip_if *);
void mg_tcpip_free(struct mg_tcpip_if *);
void mg_tcpip_qwrite(void *buf, size_t len, struct mg_tcpip_if *ifp);
size_t mg_tcpip_qread(void *buf, struct mg_tcpip_if *ifp);
// conveniency rx function for IRQ-driven drivers
size_t mg_tcpip_driver_rx(void *buf, size_t len, struct mg_tcpip_if *ifp);

extern struct mg_tcpip_driver mg_tcpip_driver_stm32;
extern struct mg_tcpip_driver mg_tcpip_driver_w5500;
extern struct mg_tcpip_driver mg_tcpip_driver_tm4c;
extern struct mg_tcpip_driver mg_tcpip_driver_stm32h;
extern struct mg_tcpip_driver mg_tcpip_driver_imxrt1020;

// Drivers that require SPI, can use this SPI abstraction
struct mg_tcpip_spi {
  void *spi;                        // Opaque SPI bus descriptor
  void (*begin)(void *);            // SPI begin: slave select low
  void (*end)(void *);              // SPI end: slave select high
  uint8_t (*txn)(void *, uint8_t);  // SPI transaction: write 1 byte, read reply
};

#if MG_ENABLE_TCPIP
#if !defined(MG_ENABLE_DRIVER_STM32H) && !defined(MG_ENABLE_DRIVER_TM4C)
#define MG_ENABLE_DRIVER_STM32 1
#else
#define MG_ENABLE_DRIVER_STM32 0
#endif
#endif
