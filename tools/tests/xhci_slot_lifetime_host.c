/* Command hardware only is modelled; Python inserts the production alloc op. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define XHCI_XFER_HOLD_BUDGET_MS 500U
#define XHCI_TRB_ENABLE_SLOT 9U
#define XHCI_TRB_ADDRESS_DEVICE 11U
#define XHCI_CC_PARAMETER_ERROR 17U
#define XHCI_TRB_TYPE(t) ((t)<<10)
typedef struct { uint8_t hcd, address, port; } usb_slot_t;
typedef struct { void *priv; } usb_hcd_t;
typedef struct { uint32_t status,control; } xhci_trb_t;
typedef struct { unsigned route; } xhci_slot_path_t;
typedef struct { unsigned initialized; } xhci_ring_t;
typedef struct {
 unsigned fatal,max_slots,context_size;
 uint32_t input_context[64],output_context[3][64];
 uint64_t dcbaa[3];xhci_ring_t ep0_ring[3];xhci_slot_path_t slot_path[3];
} xhci_controller_t;
static unsigned calls,owned,releases,which,delays;
static usb_slot_t *active_token;
static void kerr(const char *message,...) {(void)message;}
static int xhci_xfer_acquire(xhci_controller_t *x,unsigned ms) {
 (void)x;assert(ms==500&&!owned);if(which==4)return -EBUSY;owned=1;return 0;
}
static void xhci_xfer_release(xhci_controller_t *x) {(void)x;assert(owned);owned=0;releases++;}
static int xhci_slot_path(xhci_controller_t *x,unsigned port,uint8_t speed,uint8_t hub,xhci_slot_path_t *path) {
 (void)x;assert(owned&&port==1&&speed==3&&!hub);path->route=17;return which==5?-EINVAL:0;
}
static int xhci_command_locked(xhci_controller_t *x,uint64_t pointer,uint32_t status,uint32_t control,xhci_trb_t *out) {
 assert(owned&&!status);calls++;
 if(calls==1) {
  assert(!pointer&&control==XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT));
  if(which==6)return -EIO;
  out->control=(which==7?0:1)<<24;return 0;
 }
 assert(calls==2&&pointer==(uintptr_t)x->input_context);
 assert(control==(XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE)|(1U<<24)));
 assert(active_token->hcd==1&&!active_token->address&&x->dcbaa[1]);
 if(which==1||which==2||which==3) {
  out->status=(which==1?XHCI_CC_PARAMETER_ERROR:1)<<24;
  if(which==3)x->fatal=1;
  return which==3?-ETIMEDOUT:-EIO;
 }
 x->output_context[1][3]=7;return 0;
}
static uint32_t *xhci_input_context(xhci_controller_t *x,unsigned index) {return x->input_context+index*8;}
static uint32_t *xhci_output_context(xhci_controller_t *x,unsigned slot,unsigned index) {assert(slot==1&&!index);return x->output_context[slot];}
static void xhci_fill_slot_context(xhci_controller_t *x,const xhci_slot_path_t *path) {(void)x;assert(path->route==17);}
static void xhci_ring_init(xhci_ring_t *ring) {ring->initialized=1;}
static uint16_t xhci_default_mps(uint8_t speed) {assert(speed==3);return 64;}
static uint16_t xhci_mps_value(uint8_t speed,uint16_t mps) {assert(speed==3);return mps;}
static void xhci_fill_ep_context(xhci_controller_t *x,unsigned index,xhci_ring_t *ring,unsigned type,uint16_t mps,unsigned interval) {
 assert(index==1&&ring==&x->ep0_ring[1]&&ring->initialized&&type==4&&mps==64&&!interval);
}
static uintptr_t va_to_pa(void *p) {return (uintptr_t)p;}
static void arch_dma_sync_for_device(void *p,size_t n) {assert(p&&n);}
static void arch_dma_sync_for_cpu(void *p,size_t n) {assert(p&&n);}
static void mdelay(unsigned ms) {assert(ms==2);delays++;}
