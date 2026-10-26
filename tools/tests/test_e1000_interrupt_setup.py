"""Execute the production E1000 interrupt negotiation against an ownership model.

This covers routing fallback and cleanup, not a PHY or a CT5100 board NIC.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IRQF_SHARED 1
#define kinfo(...) ((void)0)
typedef struct { int dummy; } device_t;
typedef struct {
 int irq, irq_registered, poll_only, msix_base, msix_vectors, msi;
} e1000_device_t;
static int e1000_force_poll;
static int handlers[256], vectors[256], msix_live, msi_live;
static unsigned calls, ims, writes, commits, ivars, frees;
static int msix_available, vector_available, msi_available, intx_line;
static int fail_request;
static int e1000_irq_handler(int irq, void *p) { (void)irq; (void)p; return 0; }
static int e1000_msix_setup(device_t *d, e1000_device_t *n) {
 (void)d; calls++;
 if (!msix_available) return -1;
 assert(!vectors[64] && !vectors[65]); vectors[64]=vectors[65]=1;
 n->msix_base=64; n->msix_vectors=2; msix_live=1; return 0;
}
static void e1000_msix_program_ivar(e1000_device_t *n) {
 assert(msix_live && n->msix_vectors==2); ivars++;
}
static void e1000_msix_commit(device_t *d, e1000_device_t *n) {
 (void)d; assert(msix_live && handlers[n->msix_base] && handlers[n->msix_base+1]); commits++;
}
static void e1000_msix_teardown(device_t *d, e1000_device_t *n) {
 (void)d; assert(msix_live);
 /* A vector cannot be returned while a handler still owns it. */
 assert(!handlers[n->msix_base] && !handlers[n->msix_base+1]);
 vectors[n->msix_base]=vectors[n->msix_base+1]=0;
 n->msix_base=-1; n->msix_vectors=0; msix_live=0;
}
static int request_irq(uint32_t irq, int(*fn)(int,void*), unsigned flags, void *d) {
 (void)d; calls++; assert(fn==e1000_irq_handler && irq<256);
 assert(flags==(irq==11?IRQF_SHARED:0));
 if ((int)irq==fail_request) return -1;
 assert(!handlers[irq]); handlers[irq]=1; return 0;
}
static void free_irq(uint32_t irq, void *d) {
 (void)d; assert(irq<256 && handlers[irq]); handlers[irq]=0; frees++;
}
static int irq_alloc_vectors(unsigned count) {
 calls++; assert(count==1); if (!vector_available) return -1;
 assert(!vectors[80]); vectors[80]=1; return 80;
}
static void irq_free_vectors(uint32_t irq, unsigned count) {
 assert(irq==80 && count==1 && !handlers[80] && vectors[80]); vectors[80]=0;
}
static int pci_msi_enable(device_t *d, uint32_t irq) {
 (void)d; calls++; assert(irq==80 && vectors[80]);
 if (!msi_available) return -1; msi_live=1; return 0;
}
static void pci_msi_disable(device_t *d) { (void)d; assert(msi_live); msi_live=0; }
static int pci_intx_irq(device_t *d) { (void)d; calls++; return intx_line; }
static void e1000_write(e1000_device_t *n,uint32_t reg,uint32_t value) {
 assert(n->irq_registered && reg==E1000_IMS && value); ims=value; writes++;
}
'''

HARNESS = r'''
static void reset_model(void) {
 memset(handlers,0,sizeof(handlers)); memset(vectors,0,sizeof(vectors));
 msix_live=msi_live=0; calls=ims=writes=commits=ivars=frees=0;
 msix_available=vector_available=msi_available=1;
 intx_line=11; fail_request=-1; e1000_force_poll=0;
}
int main(void) {
 device_t d={0}; e1000_device_t n;
#define BEGIN() do { reset_model(); memset(&n,0,sizeof(n)); n.msix_base=-1; } while(0)
 BEGIN(); e1000_force_poll=1;
 e1000_setup_interrupts(&d,&n);
 assert(n.poll_only && !n.irq_registered && !calls && !writes);
 assert(!msix_live && !msi_live && !handlers[11] && !vectors[80]);

 BEGIN(); e1000_setup_interrupts(&d,&n);
 assert(!n.poll_only && n.irq_registered && n.irq==64 && !n.msi);
 assert(handlers[64] && handlers[65] && !handlers[80] && !handlers[11]);
 assert(commits==1 && ivars==1 && ims==E1000_IMS_MSIX && writes==1);

 BEGIN(); msix_available=0; e1000_setup_interrupts(&d,&n);
 assert(n.irq_registered && n.irq==80 && n.msi && !n.poll_only);
 assert(msi_live && handlers[80] && ims==E1000_IMS_USED && !handlers[11]);

 BEGIN(); msix_available=msi_available=0; e1000_setup_interrupts(&d,&n);
 assert(n.irq_registered && n.irq==11 && !n.msi && !n.poll_only);
 assert(handlers[11] && !vectors[80] && !msi_live && ims==E1000_IMS_USED);

 BEGIN(); msix_available=msi_available=0; intx_line=-1;
 e1000_setup_interrupts(&d,&n);
 assert(n.poll_only && !n.irq_registered && !writes && !vectors[80]);

 /* Handler 64 must be released before vectors 64/65 are freed on failure. */
 BEGIN(); fail_request=65; e1000_setup_interrupts(&d,&n);
 assert(frees==1 && !handlers[64] && !handlers[65] && !vectors[64] && !vectors[65]);
 assert(!msix_live && !n.msix_vectors && n.msi && handlers[80] && !commits);

 BEGIN(); fail_request=64; e1000_setup_interrupts(&d,&n);
 assert(!frees && !handlers[64] && !vectors[64] && n.msi && !commits);

 BEGIN(); msix_available=0; fail_request=80; e1000_setup_interrupts(&d,&n);
 assert(!msi_live && !vectors[80] && !handlers[80] && handlers[11] && !n.msi);
 assert(n.irq==11 && n.irq_registered && ims==E1000_IMS_USED);

 BEGIN(); msix_available=0; vector_available=0; e1000_setup_interrupts(&d,&n);
 assert(n.irq==11 && n.irq_registered && !vectors[80] && !msi_live);

 BEGIN(); msix_available=msi_available=0; fail_request=11;
 e1000_setup_interrupts(&d,&n);
 assert(n.poll_only && !n.irq_registered && !writes && !handlers[11]);
 puts("E1000 interrupt ownership: 10 scenarios passed");
 return 0;
}
'''


class E1000InterruptSetupTests(unittest.TestCase):
    def test_production_interrupt_negotiation(self):
        source = (ROOT / "kernel/drivers/net/e1000.c").read_text()
        start = source.index("static void e1000_setup_interrupts(")
        end = source.index("static int e1000_probe(", start)
        # Keep the real register definitions, including multiline bit masks.
        lines = source.splitlines(keepends=True)
        macros = []
        i = 0
        while i < len(lines):
            if lines[i].startswith("#define E1000_"):
                while True:
                    line = lines[i]
                    macros.append(line)
                    i += 1
                    if not line.rstrip().endswith("\\"):
                        break
            else:
                i += 1
        with tempfile.TemporaryDirectory(prefix="a20-e1000-irq-") as tmp:
            c = Path(tmp) / "test.c"
            exe = Path(tmp) / "test"
            c.write_text("".join(macros) + PRELUDE + source[start:end] + HARNESS)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-misleading-indentation", "-fsanitize=address,undefined",
                            "-fno-pie", "-no-pie", str(c), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], check=True, text=True, capture_output=True)
            self.assertIn("10 scenarios passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
