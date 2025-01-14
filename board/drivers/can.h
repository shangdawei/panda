// IRQs: CAN1_TX, CAN1_RX0, CAN1_SCE
//       CAN2_TX, CAN2_RX0, CAN2_SCE
//       CAN3_TX, CAN3_RX0, CAN3_SCE

typedef struct {
  volatile uint32_t w_ptr;
  volatile uint32_t r_ptr;
  uint32_t fifo_size;
  CAN_FIFOMailBox_TypeDef *elems;
} can_ring;

#define CAN_BUS_RET_FLAG 0x80U
#define CAN_BUS_NUM_MASK 0x7FU

#define BUS_MAX 4U

extern int can_live, pending_can_live;

// must reinit after changing these
extern int can_loopback, can_silent;
extern uint32_t can_speed[4];

void can_set_forwarding(int from, int to);

void can_init(uint8_t can_number);
void can_init_all(void);
void can_send(CAN_FIFOMailBox_TypeDef *to_push, uint8_t bus_number);
bool can_pop(can_ring *q, CAN_FIFOMailBox_TypeDef *elem);

// end API

#define ALL_CAN_SILENT 0xFF
#define ALL_CAN_BUT_MAIN_SILENT 0xFE
#define ALL_CAN_LIVE 0

int can_live = 0, pending_can_live = 0, can_loopback = 0, can_silent = ALL_CAN_SILENT;

// ********************* instantiate queues *********************

#define can_buffer(x, size) \
  CAN_FIFOMailBox_TypeDef elems_##x[size]; \
  can_ring can_##x = { .w_ptr = 0, .r_ptr = 0, .fifo_size = size, .elems = (CAN_FIFOMailBox_TypeDef *)&elems_##x };

can_buffer(rx_q, 0x1000)
can_buffer(tx1_q, 0x100)
can_buffer(tx2_q, 0x100)
can_buffer(tx3_q, 0x100)
can_buffer(txgmlan_q, 0x100)
can_ring *can_queues[] = {&can_tx1_q, &can_tx2_q, &can_tx3_q, &can_txgmlan_q};

// global CAN stats
int can_rx_cnt = 0;
int can_tx_cnt = 0;
int can_txd_cnt = 0;
int can_err_cnt = 0;
int can_overflow_cnt = 0;

// ********************* interrupt safe queue *********************

bool can_pop(can_ring *q, CAN_FIFOMailBox_TypeDef *elem) {
  bool ret = 0;

  enter_critical_section();
  if (q->w_ptr != q->r_ptr) {
    *elem = q->elems[q->r_ptr];
    if ((q->r_ptr + 1U) == q->fifo_size) {
      q->r_ptr = 0;
    } else {
      q->r_ptr += 1U;
    }
    ret = 1;
  }
  exit_critical_section();

  return ret;
}

bool can_push(can_ring *q, CAN_FIFOMailBox_TypeDef *elem) {
  bool ret = false;
  uint32_t next_w_ptr;

  enter_critical_section();
  if ((q->w_ptr + 1U) == q->fifo_size) {
    next_w_ptr = 0;
  } else {
    next_w_ptr = q->w_ptr + 1U;
  }
  if (next_w_ptr != q->r_ptr) {
    q->elems[q->w_ptr] = *elem;
    q->w_ptr = next_w_ptr;
    ret = true;
  }
  exit_critical_section();
  if (!ret) {
    can_overflow_cnt++;
    #ifdef DEBUG
      puts("can_push failed!\n");
    #endif
  }
  return ret;
}

void can_clear(can_ring *q) {
  enter_critical_section();
  q->w_ptr = 0;
  q->r_ptr = 0;
  exit_critical_section();
}

// assign CAN numbering
// bus num: Can bus number on ODB connector. Sent to/from USB
//    Min: 0; Max: 127; Bit 7 marks message as receipt (bus 129 is receipt for but 1)
// cans: Look up MCU can interface from bus number
// can number: numeric lookup for MCU CAN interfaces (0 = CAN1, 1 = CAN2, etc);
// bus_lookup: Translates from 'can number' to 'bus number'.
// can_num_lookup: Translates from 'bus number' to 'can number'.
// can_forwarding: Given a bus num, lookup bus num to forward to. -1 means no forward.

// Panda:       Bus 0=CAN1   Bus 1=CAN2   Bus 2=CAN3
CAN_TypeDef *cans[] = {CAN1, CAN2, CAN3};
uint8_t bus_lookup[] = {0,1,2};
uint8_t can_num_lookup[] = {0,1,2,-1};
int8_t can_forwarding[] = {-1,-1,-1,-1};
uint32_t can_speed[] = {5000, 5000, 5000, 333};
#define CAN_MAX 3

#define CANIF_FROM_CAN_NUM(num) (cans[num])
#define CAN_NUM_FROM_CANIF(CAN) ((CAN)==CAN1 ? 0 : ((CAN) == CAN2 ? 1 : 2))
#define CAN_NAME_FROM_CANIF(CAN) ((CAN)==CAN1 ? "CAN1" : ((CAN) == CAN2 ? "CAN2" : "CAN3"))
#define BUS_NUM_FROM_CAN_NUM(num) (bus_lookup[num])
#define CAN_NUM_FROM_BUS_NUM(num) (can_num_lookup[num])

void process_can(uint8_t can_number);

void can_set_speed(uint8_t can_number) {
  CAN_TypeDef *CAN = CANIF_FROM_CAN_NUM(can_number);
  uint8_t bus_number = BUS_NUM_FROM_CAN_NUM(can_number);

  if (!llcan_set_speed(CAN, can_speed[bus_number], can_loopback, (unsigned int)(can_silent) & (1U << can_number))) {
    puts("CAN init FAILED!!!!!\n");
    puth(can_number); puts(" ");
    puth(BUS_NUM_FROM_CAN_NUM(can_number)); puts("\n");
  }
}

void can_init(uint8_t can_number) {
  if (can_number != 0xffU) {
    CAN_TypeDef *CAN = CANIF_FROM_CAN_NUM(can_number);
    set_can_enable(CAN, 1);
    can_set_speed(can_number);

    llcan_init(CAN);

    // in case there are queued up messages
    process_can(can_number);
  }
}

void can_init_all(void) {
  for (int i=0; i < CAN_MAX; i++) {
    can_init(i);
  }
}

void can_set_gmlan(uint8_t bus) {

  // first, disable GMLAN on prev bus
  uint8_t prev_bus = can_num_lookup[3];
  if (bus != prev_bus) {
    switch (prev_bus) {
      case 1:
      case 2:
        puts("Disable GMLAN on CAN");
        puth(prev_bus + 1U);
        puts("\n");
        set_can_mode(prev_bus, 0);
        bus_lookup[prev_bus] = prev_bus;
        can_num_lookup[prev_bus] = prev_bus;
        can_num_lookup[3] = -1;
        can_init(prev_bus);
        break;
      default:
        // GMLAN was not set on either BUS 1 or 2
        break;
    }
  }

  // now enable GMLAN on the new bus
  switch (bus) {
    case 1:
    case 2:
      puts("Enable GMLAN on CAN");
      puth(bus + 1U);
      puts("\n");
      set_can_mode(bus, 1);
      bus_lookup[bus] = 3;
      can_num_lookup[bus] = -1;
      can_num_lookup[3] = bus;
      can_init(bus);
      break;
    default:
      puts("GMLAN can only be set on CAN2 or CAN3");
      break;
  }
}

// CAN error
void can_sce(CAN_TypeDef *CAN) {
  enter_critical_section();

  #ifdef DEBUG
    if (CAN==CAN1) puts("CAN1:  ");
    if (CAN==CAN2) puts("CAN2:  ");
    #ifdef CAN3
      if (CAN==CAN3) puts("CAN3:  ");
    #endif
    puts("MSR:");
    puth(CAN->MSR);
    puts(" TSR:");
    puth(CAN->TSR);
    puts(" RF0R:");
    puth(CAN->RF0R);
    puts(" RF1R:");
    puth(CAN->RF1R);
    puts(" ESR:");
    puth(CAN->ESR);
    puts("\n");
  #endif

  can_err_cnt += 1;
  llcan_clear_send(CAN);
  exit_critical_section();
}

// ***************************** CAN *****************************

void process_can(uint8_t can_number) {
  if (can_number != 0xffU) {

    enter_critical_section();

    CAN_TypeDef *CAN = CANIF_FROM_CAN_NUM(can_number);
    uint8_t bus_number = BUS_NUM_FROM_CAN_NUM(can_number);

    // check for empty mailbox
    CAN_FIFOMailBox_TypeDef to_send;
    if ((CAN->TSR & CAN_TSR_TME0) == CAN_TSR_TME0) {
      // add successfully transmitted message to my fifo
      if ((CAN->TSR & CAN_TSR_RQCP0) == CAN_TSR_RQCP0) {
        can_txd_cnt += 1;

        if ((CAN->TSR & CAN_TSR_TXOK0) == CAN_TSR_TXOK0) {
          CAN_FIFOMailBox_TypeDef to_push;
          to_push.RIR = CAN->sTxMailBox[0].TIR;
          to_push.RDTR = (CAN->sTxMailBox[0].TDTR & 0xFFFF000FU) | ((CAN_BUS_RET_FLAG | bus_number) << 4);
          to_push.RDLR = CAN->sTxMailBox[0].TDLR;
          to_push.RDHR = CAN->sTxMailBox[0].TDHR;
          can_push(&can_rx_q, &to_push);
        }

        if ((CAN->TSR & CAN_TSR_TERR0) == CAN_TSR_TERR0) {
          #ifdef DEBUG
            puts("CAN TX ERROR!\n");
          #endif
        }

        if ((CAN->TSR & CAN_TSR_ALST0) == CAN_TSR_ALST0) {
          #ifdef DEBUG
            puts("CAN TX ARBITRATION LOST!\n");
          #endif
        }

        // clear interrupt
        // careful, this can also be cleared by requesting a transmission
        CAN->TSR |= CAN_TSR_RQCP0;
      }

      if (can_pop(can_queues[bus_number], &to_send)) {
        can_tx_cnt += 1;
        // only send if we have received a packet
        CAN->sTxMailBox[0].TDLR = to_send.RDLR;
        CAN->sTxMailBox[0].TDHR = to_send.RDHR;
        CAN->sTxMailBox[0].TDTR = to_send.RDTR;
        CAN->sTxMailBox[0].TIR = to_send.RIR;
      }
    }

    exit_critical_section();
  }
}

// CAN receive handlers
// blink blue when we are receiving CAN messages
void can_rx(uint8_t can_number) {
  CAN_TypeDef *CAN = CANIF_FROM_CAN_NUM(can_number);
  uint8_t bus_number = BUS_NUM_FROM_CAN_NUM(can_number);
  while ((CAN->RF0R & CAN_RF0R_FMP0) != 0) {
    can_rx_cnt += 1;

    // can is live
    pending_can_live = 1;

    // add to my fifo
    CAN_FIFOMailBox_TypeDef to_push;
    to_push.RIR = CAN->sFIFOMailBox[0].RIR;
    to_push.RDTR = CAN->sFIFOMailBox[0].RDTR;
    to_push.RDLR = CAN->sFIFOMailBox[0].RDLR;
    to_push.RDHR = CAN->sFIFOMailBox[0].RDHR;

    // modify RDTR for our API
    to_push.RDTR = (to_push.RDTR & 0xFFFF000F) | (bus_number << 4);

    // forwarding (panda only)
    int bus_fwd_num = (can_forwarding[bus_number] != -1) ? can_forwarding[bus_number] : safety_fwd_hook(bus_number, &to_push);
    if (bus_fwd_num != -1) {
      CAN_FIFOMailBox_TypeDef to_send;
      to_send.RIR = to_push.RIR | 1; // TXRQ
      to_send.RDTR = to_push.RDTR;
      to_send.RDLR = to_push.RDLR;
      to_send.RDHR = to_push.RDHR;
      can_send(&to_send, bus_fwd_num);
    }

    safety_rx_hook(&to_push);

    set_led(LED_BLUE, 1);
    can_push(&can_rx_q, &to_push);

    // next
    CAN->RF0R |= CAN_RF0R_RFOM0;
  }
}

void CAN1_TX_IRQHandler(void) { process_can(0); }
void CAN1_RX0_IRQHandler(void) { can_rx(0); }
void CAN1_SCE_IRQHandler(void) { can_sce(CAN1); }

void CAN2_TX_IRQHandler(void) { process_can(1); }
void CAN2_RX0_IRQHandler(void) { can_rx(1); }
void CAN2_SCE_IRQHandler(void) { can_sce(CAN2); }

void CAN3_TX_IRQHandler(void) { process_can(2); }
void CAN3_RX0_IRQHandler(void) { can_rx(2); }
void CAN3_SCE_IRQHandler(void) { can_sce(CAN3); }

void can_send(CAN_FIFOMailBox_TypeDef *to_push, uint8_t bus_number) {
  if (safety_tx_hook(to_push) != 0) {
    if (bus_number < BUS_MAX) {
      // add CAN packet to send queue
      // bus number isn't passed through
      to_push->RDTR &= 0xF;
      if ((bus_number == 3U) && (can_num_lookup[3] == 0xFFU)) {
        // TODO: why uint8 bro? only int8?
        bitbang_gmlan(to_push);
      } else {
        can_push(can_queues[bus_number], to_push);
        process_can(CAN_NUM_FROM_BUS_NUM(bus_number));
      }
    }
  }
}

void can_set_forwarding(int from, int to) {
  can_forwarding[from] = to;
}

