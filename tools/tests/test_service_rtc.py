"""Exercise the real PL031 service protocol with a register-level fake."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ServiceRTC(unittest.TestCase):
    def test_pl031_time_alarm_clear_and_wrap(self):
        with tempfile.TemporaryDirectory(prefix="a20-pl031-") as tmp:
            work = Path(tmp)
            header = work / "drivers/dual/drv_env.h"
            header.parent.mkdir(parents=True)
            header.write_text("""
#include <stdint.h>
#include <assert.h>
static uint32_t counter, offsets[16], values[16], writes;
static uint64_t drv_mmio_map(uint64_t pa, uint64_t size, unsigned prot) {
    assert(pa==0x09010000 && size==4096 && prot==3); return 0x100000;
}
static uint32_t drv_mmio_read32(uint64_t base, unsigned off) {
    assert(base==0x100000 && off==0); return counter;
}
static void drv_mmio_write32(uint64_t base, unsigned off, uint32_t value) {
    assert(base==0x100000 && writes<16);
    offsets[writes]=off; values[writes++]=value;
}
""")
            source = work / "test.c"
            source.write_text("""
#include "drivers/dual/service_rtc.h"
int main(void) {
    uint64_t base=service_rtc_map();
    assert(SERVICE_RTC_IRQ==34);
    counter=1794777003;
    assert(service_rtc_read_ns(base)==1794777003000000000ULL);
    service_rtc_set_alarm_ns(base,service_rtc_read_ns(base)+100000000);
    assert(writes==4);
    assert(offsets[0]==0x10 && values[0]==0);
    assert(offsets[1]==0x1c && values[1]==1);
    assert(offsets[2]==4 && values[2]==counter+2);
    assert(offsets[3]==0x10 && values[3]==1);
    /* No fractional counter phase can make this 100ms alarm early. */
    for(uint64_t phase=0;phase<1000000000ULL;phase+=1000000)
        assert((uint64_t)values[2]*1000000000ULL -
               (service_rtc_read_ns(base)+phase)>=100000000ULL);
    service_rtc_clear_alarm(base);
    assert(writes==6 && offsets[4]==0x10 && values[4]==0);
    assert(offsets[5]==0x1c && values[5]==1);
    writes=0; counter=UINT32_MAX;
    service_rtc_set_alarm_ns(base,service_rtc_read_ns(base)+100000000);
    assert(values[2]==1); /* PL031's unsigned seconds counter wraps. */
    writes=0;
    service_rtc_set_alarm_ns(base,service_rtc_read_ns(base)+1000000000);
    assert(values[2]==1);
    return 0;
}
""")
            binary = work / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-D__aarch64__", "-I" + str(work),
                            "-I" + str(ROOT / "kernel/include"), str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
