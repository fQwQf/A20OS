"""Read-only GPIO adaptation: provider identity, bounds and polarity."""
from pathlib import Path
import re
import unittest
from tools.tests.test_i2c_transport_lifetime import run, ROOT


class FirmwareGpioInput(unittest.TestCase):
    def test_real_adapter_rejects_unknown_and_out_of_range_resources(self):
        source = (ROOT / 'kernel/drivers/gpio/firmware_input.c').read_text()
        source = re.sub(r'^#include.*\n', '', source, flags=re.M)
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef struct {int unused;} fw_node_t;
typedef struct {fw_node_t node;uint32_t args[16];size_t count;} fw_reference_t;
typedef struct {int type;uint64_t address,size;} fw_resource_t;
typedef struct {const volatile uint32_t *reg;uint32_t mask;uint8_t active_low;} gpio_firmware_input_t;
#define FW_RESOURCE_MEMORY 0
static fw_reference_t ref={.args={3,1},.count=2};
static fw_resource_t res={.address=0x1000,.size=32};
static unsigned maps;static int supported=1,ref_error;static uint32_t value;
static int fw_node_reference(const fw_node_t*n,const char*p,const char*c,size_t i,fw_reference_t*out){(void)n;(void)p;(void)c;assert(!i);*out=ref;return ref_error;}
static int fw_node_match_id(const fw_node_t*n,const char*id){(void)n;assert(!strcmp(id,"AMDI0030"));return supported;}
static int fw_node_resource(const fw_node_t*n,size_t i,fw_resource_t*out){(void)n;if(i)return -ENOENT;*out=res;return 0;}
static void *arch_device_map_mmio(uint64_t pa,size_t size,uint32_t flags){assert(pa==0x100c&&size==4&&!flags);maps++;return &value;}
static uint32_t readl(const volatile uint32_t *p){return *p;}
''' + source + r'''
int main(void){
 fw_node_t node={0};gpio_firmware_input_t line;
 assert(!gpio_firmware_input_get(&node,"attention",&line)&&maps==1);
 value=1U<<16;assert(!gpio_firmware_input_pending(&line));value=0;assert(gpio_firmware_input_pending(&line));
 ref.args[1]=0;assert(!gpio_firmware_input_get(&node,"attention",&line));
 assert(!gpio_firmware_input_pending(&line));value=1U<<16;assert(gpio_firmware_input_pending(&line));
 supported=0;assert(gpio_firmware_input_get(&node,"attention",&line)==-EOPNOTSUPP&&!line.reg&&maps==2);
 assert(gpio_firmware_input_pending(&line));supported=1;
 ref.args[1]=2;assert(gpio_firmware_input_get(&node,"attention",&line)==-EINVAL);
 ref.args[1]=0;res.size=15;assert(gpio_firmware_input_get(&node,"attention",&line)==-ERANGE);
 res.size=32;res.address=UINT64_MAX-1;assert(gpio_firmware_input_get(&node,"attention",&line)==-ERANGE);
 res.address=0x1000;ref_error=-ENOENT;assert(gpio_firmware_input_get(&node,"attention",&line)==-ENOENT);
 assert(maps==2);
}
''')


if __name__ == '__main__':
    unittest.main()
