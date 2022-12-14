#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  // TX
  // _____________
  // |          dd
  // |          dd
  // |          dd  <- H == T
  // |          dd
  // |          dd
  // |          dd

  // TX
  // _____________
  // |          dd
  // |          dd
  // | xxxxxx       <- H == T   -- Pozrieme sa na T, ci okruh nepretiekol, ci nie je DD == 0
  // |          dd              -- Ak je DD = 1, vsetko je v poriadku
  // |          dd              -- Zapise sa m->head, m->len, nastavit CMD priznaky EOP a RS
  // |          dd              -- Aktualizovat poziciu okruhu

  // H - deskriptor na ktorom pracuje hardver
  // T - je vzdy za poslednym deskriptorom ku ktoremu ma HW pristup

   // TX
  // _____________
  // | xxxxxx         
  // |   xxxx       
  // | xxxxx        <- H == T   -- Tento deskriptor nema nastaveny DD (Descriptor Done bit)
  // | xxxxxx                   -- Funguje to ako ochrana pred pretecenim
  // |   xxxx       
  // | xxxxx

  // Zamikanie
  // Vyziadanie indexu okruhu
  // Kontrola DD ci nepretiekol, ak ano uvolnime zamok

  acquire(&e1000_lock);
  uint64 index = regs[E1000_TDT];
  if ((tx_ring[index].status & E1000_TXD_STAT_DD) == 0) {
    release(&e1000_lock);
    return -1;
  }
  // Uvolnime buffer ak na danom indexe nieco je
  if (tx_mbufs[index] != 0) {
    mbuffree(tx_mbufs[index]);
  }

  // Vyplnanie deskriptora TX, addr -> jead, length -> len, pripocitanie cmd priznakov EOP RS
  // Ukladanie na neskorsie uvolneine
  // Aktualizacia pozicie, posun o jedno nizsie modulo
  // Uvolnenie zamku
  tx_ring[index].addr = (uint64) m->head;
  tx_ring[index].length = m->len;
  tx_ring[index].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tx_mbufs[index] = m;
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;
  release(&e1000_lock);


  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // RX
  // _________
  // |          <- H  Tam kde je H bude zapisany prijimany packet, vsetko medzi H a T patri HW
  // |
  // |
  // |
  // |
  // |          <- T

  // RX
  // _________
  // | xxxxxx dd         
  // |            <- H  - ked karta zapise paket posunie HEAD nizsie
  // |                  - Ak H == T - fronta je uz prazdna (plna tabulka)
  // |                  - DD bit bude nastavovany na deskripory, ktore su pripravene na precitanie
  // |                  - DD - deskriptor s prijatym paketom je pripraveny na precitanie
  // |            <- T


  // Vyziadanie indexu okruhu, kde sa caka na dalsi paket
  // RD + 1 % RING_SIZE
  // Kontrola DD

  acquire(&e1000_lock);
  uint64 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

  struct mbuf *buf[RX_RING_SIZE];
  for (uint64 i = 0; i < RX_RING_SIZE; i++) {
    buf[i] = 0;
  }
  uint64 i = 0;
  while (index != regs[E1000_RDH]) {
    if ((rx_ring[index].status & E1000_RXD_STAT_DD) == 0) {
      // Neskor sa uvolni zamok
      break;
    }
    rx_mbufs[index]->len = rx_ring[index].length;
    buf[i++] = rx_mbufs[index];           // odoslany do net_rx

    struct mbuf *newBuf = mbufalloc(0);   // alokovanie noveho buffera

    rx_ring[index].addr = (uint64) newBuf->head;
    rx_mbufs[index] = newBuf;
    regs[E1000_RDT] = index;              // nastavenie registra na posledny spracovany deskriptor
    index = (index + 1) % RX_RING_SIZE;
  }
  release(&e1000_lock);

  i = 0;
  while (buf[i] != 0) {
    net_rx(buf[i]);
    i = (i + 1) % RX_RING_SIZE;
  }
  return;
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
