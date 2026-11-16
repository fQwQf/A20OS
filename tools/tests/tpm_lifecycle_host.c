/* Production module, simulated TIS registers. No real TPM/firmware certification. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define A20_DRIVER_DESCRIPTOR(...)
#define MS_TO_TICKS(ms) ((uint64_t)(ms))
#define RES_MMIO 1
#define DEV_CLASS_NONE 0
static void host_log(const char *format,...){(void)format;}
#define kinfo host_log
#define kerr host_log
typedef struct {int held;} mutex_t;
#define MUTEX_INIT {0}
static void mutex_lock(mutex_t *m){assert(!m->held);m->held=1;}
static void mutex_unlock(mutex_t *m){assert(m->held);m->held=0;}
static int mutex_trylock(mutex_t *m){if(m->held)return 0;m->held=1;return 1;}
typedef struct {int acpi;unsigned method;} firmware_node_t;
typedef struct {uint64_t start,end;} resource_t;
typedef struct device {
    void *drv_priv;firmware_node_t firmware_node;resource_t mmio;
    int managed,mapped;int(*execution)(void*);void *context;
} device_t;
typedef struct {const char *id;} firmware_device_id_t;
typedef struct {
    const char *name;const firmware_device_id_t *firmware_ids;void *bus;
    int(*probe)(device_t*);int(*remove)(device_t*);int class_type;
} driver_t;
static int platform_bus;
static uint8_t readb(const volatile void *p);
static uint32_t readl(const volatile void *p);
static void writeb(uint8_t v,volatile void *p);
static void writel(uint32_t v,volatile void *p);
static uint64_t timer_get_ticks(void);
static void arch_cpu_relax(void);
static int fw_node_match_id(firmware_node_t *node,const char *id);
static int fw_node_read_u32(firmware_node_t *node,const char *key,uint32_t *value);
static resource_t *device_get_resource(device_t *dev,int type,int index);
static void *device_map_mmio(device_t *dev,int index);
static int device_probe_use_remove(device_t *dev);
static int device_add_execution_action(device_t *dev,int(*fn)(void*),void(*release)(void*),void *context);
static int drv_driver_register(driver_t *driver);
#define drv_log host_log
/* PRODUCTION */
#define MODEL_BASE 0x10000000U
static device_t dev;
static uint64_t ticks;
static uint8_t access_reg,status_reg,command[128],response[128];
static unsigned command_count,response_count,response_pos,go_count,unmaps,reads,writes;
static uint32_t irq_enable,startup_code,random_code;
static int dead,deny_locality,stuck_mask,stuck_ready,stuck_release;
static int zero_send_burst,zero_recv_burst,no_response,early_expect,late_expect;
static int fail_action,fail_map,cancel_on_poll,fail_did;
static unsigned random_limit=64,declared_length,actual_limit,random_size_override;
static int bad_tag,trailing,in_response,registered;

static uint32_t offset(const volatile void *p){
    assert(dev.mapped && tpm_lock.held);
    uintptr_t address=(uintptr_t)p;assert(address>=MODEL_BASE && address<MODEL_BASE+4096);
    return address-MODEL_BASE;
}
static uint64_t timer_get_ticks(void){return ticks;}
static void arch_cpu_relax(void){
    ticks++;assert(ticks<100000);
    if(cancel_on_poll){cancel_on_poll=0;assert(tpm_stop(&dev)==-EBUSY);}
}
static uint8_t readb(const volatile void *p){
    unsigned reg=offset(p);reads++;
    if(dead)return 0xff;
    if(reg==TIS_ACCESS)return access_reg;
    if(reg==TIS_STS)return status_reg;
    if(reg==TIS_RID)return 1;
    assert(reg==TIS_DATA_FIFO && in_response && response_pos<response_count);
    uint8_t byte=response[response_pos++];
    if(response_pos==response_count)status_reg&=~TIS_STS_DATA_AVAIL;
    return byte;
}
static uint32_t readl(const volatile void *p){
    unsigned reg=offset(p);reads++;
    if(dead)return UINT32_MAX;
    if(reg==TIS_DID_VID)return fail_did?0:0x00011014;
    if(reg==TIS_INT_ENABLE)return irq_enable;
    assert(reg==TIS_STS); /* FIFO/ACCESS/STS writes and FIFO reads must be bytes. */
    unsigned burst=(in_response?zero_recv_burst:zero_send_burst)?0:64;
    return status_reg|(burst<<8);
}
static void make_response(void){
    assert(command_count==tpm_be32(command+2) && command_count>=10);
    uint32_t cc=tpm_be32(command+6),rc;
    memset(response,0,sizeof(response));
    response_count=10;unsigned amount=0;
    if(cc==TPM2_CC_STARTUP){assert(command_count==12);rc=startup_code;}
    else {
        assert(cc==TPM2_CC_GET_RANDOM && command_count==12);
        amount=((unsigned)command[10]<<8)|command[11];
        if(amount>random_limit)amount=random_limit;
        rc=random_code;
        if(!rc){
            response_count=12+amount;
            unsigned encoded=random_size_override?random_size_override:amount;
            response[10]=encoded>>8;response[11]=encoded;
            for(unsigned i=0;i<amount;i++)response[12+i]=0xa0+i;
        }
    }
    response[0]=0x80;response[1]=bad_tag?2:1;
    tpm_put_be32(response+2,declared_length?declared_length:response_count);
    tpm_put_be32(response+6,rc);
    if(trailing)response_count++;
    if(actual_limit && response_count>actual_limit)response_count=actual_limit;
    response_pos=0;in_response=1;
    status_reg=TIS_STS_VALID|(no_response?0:TIS_STS_DATA_AVAIL);
}
static void writeb(uint8_t v,volatile void *p){
    unsigned reg=offset(p);writes++;
    assert(dev.managed && dev.execution);
    if(dead)return;
    if(reg==TIS_ACCESS){
        if(v==TIS_ACCESS_REQUEST_USE){
            access_reg=TIS_ACCESS_VALID|(deny_locality?TIS_ACCESS_REQUEST_USE:TIS_ACCESS_ACTIVE_LOCALITY);
        }else {
            assert(v==TIS_ACCESS_ACTIVE_LOCALITY);
            assert((status_reg&TIS_STS_COMMAND_READY) && !(status_reg&TIS_STS_DATA_AVAIL));
            if(!stuck_release)access_reg=TIS_ACCESS_VALID;
        }
        return;
    }
    assert(access_reg&TIS_ACCESS_ACTIVE_LOCALITY);
    if(reg==TIS_STS){
        if(v==TIS_STS_COMMAND_READY){
            if(!stuck_ready){
                status_reg=TIS_STS_VALID|TIS_STS_COMMAND_READY;
                command_count=response_pos=response_count=0;in_response=0;
            }
        }else {
            assert(v==TIS_STS_GO && !(status_reg&TIS_STS_DATA_EXPECT));
            go_count++;make_response();
        }
        return;
    }
    assert(reg==TIS_DATA_FIFO && command_count<sizeof(command));
    command[command_count++]=v;status_reg=TIS_STS_VALID;
    if(command_count<6 || command_count<tpm_be32(command+2) || late_expect)
        status_reg|=TIS_STS_DATA_EXPECT;
    if(early_expect)status_reg&=~TIS_STS_DATA_EXPECT;
}
static void writel(uint32_t v,volatile void *p){
    assert(offset(p)==TIS_INT_ENABLE && (access_reg&TIS_ACCESS_ACTIVE_LOCALITY));writes++;
    if(!stuck_mask)irq_enable=v;
}
static int fw_node_match_id(firmware_node_t *node,const char *id){
    assert(!strcmp(id,"MSFT0101"));return node->acpi;
}
static int fw_node_read_u32(firmware_node_t *node,const char *key,uint32_t *value){
    assert(!strcmp(key,"acpi,tpm2-start-method"));*value=node->method;return 0;
}
static resource_t *device_get_resource(device_t *d,int type,int index){assert(type==RES_MMIO && !index);return &d->mmio;}
static void *device_map_mmio(device_t *d,int index){
    assert(d==&dev && d->managed && d->execution && !index);
    if(fail_map)return NULL;
    d->mapped=1;return (void *)(uintptr_t)MODEL_BASE;
}
static int device_probe_use_remove(device_t *d){assert(tpm_driver.remove==tpm_remove);d->managed=1;return 0;}
static int device_add_execution_action(device_t *d,int(*fn)(void*),void(*release)(void*),void *context){
    assert(d->managed && d->drv_priv==&g_tpm && context==d && !release && !g_tpm.base);
    if(fail_action)return -ENOMEM;
    d->execution=fn;d->context=context;return 0;
}
static int drv_driver_register(driver_t *d){assert(d==&tpm_driver);registered++;return 0;}
static int core_remove(void){
    /* Model only the existing core stop/remove/devres ordering. */
    int result=dev.execution?dev.execution(dev.context):0;
    if(!result)result=tpm_driver.remove(&dev);
    if(result){assert(dev.drv_priv==&g_tpm && g_tpm.owner==&dev);return result;}
    if(dev.mapped){dev.mapped=0;unmaps++;}
    dev.execution=NULL;assert(!g_tpm.base && !g_tpm.owner && !dev.drv_priv);return 0;
}
static void fresh(void){
    assert(!dev.mapped && !g_tpm.owner && !g_tpm.base);
    memset(&dev,0,sizeof(dev));dev.mmio=(resource_t){0xfed40000,0xfed40fff};
    dev.firmware_node=(firmware_node_t){1,6};
    access_reg=TIS_ACCESS_VALID;status_reg=TIS_STS_VALID|TIS_STS_COMMAND_READY;
    irq_enable=TIS_GLOBAL_INT_ENABLE|5;startup_code=random_code=0;
    dead=deny_locality=stuck_mask=stuck_ready=stuck_release=0;
    zero_send_burst=zero_recv_burst=no_response=early_expect=late_expect=0;
    fail_action=fail_map=cancel_on_poll=fail_did=0;
    declared_length=actual_limit=random_size_override=0;
    bad_tag=trailing=in_response=0;random_limit=64;ticks=0;
}
static void expect_unchanged(uint8_t *buffer,int error){
    memset(buffer,0x5a,64);assert(tpm_get_random(buffer,37)==error);
    for(unsigned i=0;i<64;i++)assert(buffer[i]==0x5a);
}
int main(void){
    assert(!DriverEntry() && registered==1);fresh();unsigned goes=go_count;
    assert(!tpm_probe(&dev) && tpm_present() && go_count==goes+1);
    assert(!(irq_enable&TIS_GLOBAL_INT_ENABLE) && access_reg==TIS_ACCESS_VALID);
    device_t second=dev;assert(tpm_probe(&second)==-EBUSY);
    uint8_t output[64];memset(output,0,64);
    assert(tpm_get_random(output,37)==37 && go_count==goes+2);
    for(unsigned i=0;i<37;i++)assert(output[i]==(uint8_t)(0xa0+i));
    random_limit=5;assert(tpm_get_random(output,37)==5);random_limit=64;
    random_code=0x922;expect_unchanged(output,-EIO);random_code=0;
    random_size_override=64;expect_unchanged(output,-EIO);random_size_override=0;
    declared_length=9;expect_unchanged(output,-EIO);
    declared_length=100;expect_unchanged(output,-EIO);declared_length=0;
    bad_tag=1;expect_unchanged(output,-EIO);bad_tag=0;
    actual_limit=10;expect_unchanged(output,-ETIMEDOUT);actual_limit=0;
    trailing=1;expect_unchanged(output,-EIO);trailing=0;
    early_expect=1;expect_unchanged(output,-EIO);early_expect=0;
    late_expect=1;expect_unchanged(output,-EIO);late_expect=0;
    uint64_t before=ticks;zero_send_burst=1;expect_unchanged(output,-ETIMEDOUT);
    assert(ticks-before==TIS_TIMEOUT_A_MS);zero_send_burst=0;
    zero_recv_burst=1;expect_unchanged(output,-ETIMEDOUT);zero_recv_burst=0;
    no_response=1;expect_unchanged(output,-ETIMEDOUT);no_response=0;
    uint8_t cmd[12],rsp[80];uint32_t length=99;
    tpm2_cmd_hdr(cmd,TPM2_CC_GET_RANDOM,12);
    assert(tpm_send_command(cmd,0,rsp,80,&length)==-EINVAL);
    assert(tpm2_command(NULL,UINT32_MAX,TPM2_CC_GET_RANDOM,rsp,80,&length)==UINT32_MAX);
    tpm2_cmd_hdr(rsp,0x922,10);assert(tpm2_response_code(rsp,10)==0x922);
    assert(!core_remove() && !tpm_present());
    unsigned old_reads=reads;expect_unchanged(output,-ENODEV);assert(reads==old_reads);

    fresh();startup_code=TPM2_RC_INITIALIZE;assert(!tpm_probe(&dev));assert(!core_remove());
    fresh();startup_code=0x101;assert(tpm_probe(&dev)==-EIO && !tpm_present());assert(!core_remove());
    fresh();fail_action=1;unsigned old_writes=writes;
    assert(tpm_probe(&dev)==-ENOMEM && writes==old_writes && !dev.mapped);assert(!core_remove());
    fresh();fail_map=1;assert(tpm_probe(&dev)==-EIO && !dev.mapped);assert(!core_remove());
    fresh();fail_did=1;old_writes=writes;
    assert(tpm_probe(&dev)==-ENODEV && writes==old_writes);assert(!core_remove());
    fresh();deny_locality=1;assert(tpm_probe(&dev)==-ETIMEDOUT && dev.mapped);
    unsigned old_unmaps=unmaps;assert(core_remove()==-ETIMEDOUT && unmaps==old_unmaps);
    deny_locality=0;assert(!core_remove() && unmaps==old_unmaps+1);
    fresh();stuck_mask=1;assert(tpm_probe(&dev)==-EIO && dev.mapped && !tpm_present());
    assert(core_remove()==-EIO);stuck_mask=0;assert(!core_remove());

    fresh();assert(!tpm_probe(&dev));stuck_ready=1;
    expect_unchanged(output,-ETIMEDOUT);
    assert(g_tpm.closing && g_tpm.locality_owned && !tpm_present() && dev.mapped);
    assert(core_remove()==-ETIMEDOUT);stuck_ready=0;assert(!core_remove());

    fresh();assert(!tpm_probe(&dev));
    zero_send_burst=1;cancel_on_poll=1;expect_unchanged(output,-ECANCELED);
    assert(g_tpm.closing && !tpm_present() && dev.mapped);zero_send_burst=0;
    assert(!core_remove());
    fresh();assert(!tpm_probe(&dev));old_unmaps=unmaps;
    mutex_lock(&tpm_lock);assert(core_remove()==-EBUSY);mutex_unlock(&tpm_lock);
    assert(!tpm_present() && dev.mapped && unmaps==old_unmaps);
    irq_enable|=TIS_GLOBAL_INT_ENABLE;stuck_mask=1;
    assert(core_remove()==-EIO && unmaps==old_unmaps);stuck_mask=0;
    status_reg=TIS_STS_VALID|TIS_STS_DATA_AVAIL;stuck_ready=1;
    assert(core_remove()==-ETIMEDOUT && unmaps==old_unmaps);stuck_ready=0;
    stuck_release=1;assert(core_remove()==-ETIMEDOUT && dev.mapped);stuck_release=0;
    dead=1;assert(core_remove()==-ENODEV && unmaps==old_unmaps);dead=0;
    assert(!core_remove() && unmaps==old_unmaps+1);
    assert(!core_remove() && unmaps==old_unmaps+1);
    fresh();assert(!tpm_probe(&dev) && tpm_present());assert(!core_remove());
    fresh();dev.firmware_node.method=7;old_writes=writes;
    assert(tpm_probe(&dev)==-EOPNOTSUPP && !dev.drv_priv && writes==old_writes);
    return 0;
}
