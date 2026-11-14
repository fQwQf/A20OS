/* Included after the common production-core host boundary definitions. */
#include "drivers/core/protocol.h"
#include "drivers/net/interface.h"

#define TEST_SERVICE 0x8000000000000011ULL
struct service_ops { int (*read)(device_t *, int *); };
static unsigned mode, generation = 1;
static int remove_failure, active_value = 41;
static int service_read(device_t *dev, int *out)
{ assert(dev->drv_priv == &active_value); *out = *(int *)dev->drv_priv; return 0; }
static const struct service_ops service = {service_read};
static int stats_read(device_t *dev, net_dev_stats_t *out)
{ assert(dev->drv_priv); *out = (net_dev_stats_t){1,2,3,4}; return 0; }
static const struct driver_net_stats_ops stats = {stats_read};
static int execution_failure, stops[2], releases;
static struct driver_protocol_access *execution_held;
static int execution_stop(void *context)
{
    unsigned index=(uintptr_t)context;
    stops[index]++;
    if (!index && execution_failure) return -ETIMEDOUT;
    if (!index && execution_held) { driver_protocol_end(execution_held);execution_held=NULL; }
    return 0;
}
static void execution_release(void *context) { (void)context;releases++; }
static int protocol_probe(device_t *dev)
{
    const struct driver_protocol_descriptor descriptor = {
        .info = {TEST_SERVICE, generation, sizeof(service), 3}, .ops = &service,
    };
    assert(!device_add_protocol(dev, &descriptor));
    assert(device_add_protocol(dev, &descriptor) == -EEXIST);
    struct driver_protocol_descriptor invalid = descriptor;
    invalid.info.ops_size = 0; assert(device_add_protocol(dev,&invalid)==-EINVAL);
    invalid = descriptor; invalid.ops = NULL; assert(device_add_protocol(dev,&invalid)==-EINVAL);
    if (dev->drv->class_type == DEV_CLASS_NET) {
        const struct driver_protocol_descriptor network = {
            .info={DRIVER_PROTOCOL_NET_STATS,1,sizeof(stats),0}, .ops=&stats,
        };
        assert(!device_add_protocol(dev,&network));
        invalid=descriptor;invalid.info.id=DRIVER_PROTOCOL_NET;
        assert(device_add_protocol(dev,&invalid)==-EEXIST);
    }
    dev->drv_priv=&active_value;
    if (mode>=3) {
        if (mode==5) fail_next=1;
        int result=device_add_execution_action(dev,execution_stop,execution_release,(void *)0);
        if (mode==5) { assert(result==-ENOMEM);return -EIO; }
        assert(!result);
        assert(!device_add_execution_action(dev,execution_stop,execution_release,(void *)1));
        if (mode==4) return -EIO;
    }
    if (mode==1) fail_next=1; /* Publication allocation, after metadata staging. */
    if (mode==2) { assert(!device_probe_use_remove(dev)); return -EIO; }
    return 0;
}
static int protocol_remove(device_t *dev)
{
    if (remove_failure) return -ETIMEDOUT;
    dev->drv_priv=NULL;__atomic_add_fetch(&removed,1,__ATOMIC_RELEASE);return 0;
}
static driver_t implementation = {
    .name="protocol-service",.match=match,.probe=protocol_probe,.remove=protocol_remove,
};
static device_t *start(void)
{
    assert(!driver_register(&implementation));
    device_t *dev=calloc(1,sizeof(*dev));assert(dev);
    dev->name="test-device";dev->release=dev_release;
    assert(!device_register(dev));return dev;
}
static struct driver_protocol_requirement require_service = {TEST_SERVICE,1,sizeof(service),1};
static void snapshot_and_access(device_t *dev, driver_endpoint_t *endpoint, unsigned expected)
{
    assert(endpoint && driver_endpoint_protocol_count(endpoint)==expected);
    struct driver_protocol_info previous={0},info;
    for (unsigned i=0;i<expected;i++) {
        assert(!driver_endpoint_protocol_info(endpoint,i,&info));
        assert(info.id>previous.id && info.version && info.ops_size);previous=info;
    }
    info=(struct driver_protocol_info){.id=99};
    assert(driver_endpoint_protocol_info(endpoint,expected,&info)==-EINVAL && !info.id);
    struct driver_protocol_access access;
    fail_next=1; /* Data path performs neither metadata allocation nor reclaim. */
    assert(!driver_protocol_begin(endpoint,&require_service,&access));
    assert(fail_next && access.endpoint==endpoint && access.device==dev && access.ops==&service);
    int value=0;assert(!((const struct service_ops *)access.ops)->read(access.device,&value));
    assert(value==41);driver_protocol_end(&access);assert(!access.ops && !access.endpoint);
    driver_protocol_end(&access);fail_next=0;
    struct driver_protocol_requirement bad=require_service;
    bad.version=2;assert(driver_protocol_begin(endpoint,&bad,&access)==-EPROTONOSUPPORT);
    bad=require_service;bad.ops_size++;assert(driver_protocol_begin(endpoint,&bad,&access)==-EOPNOTSUPP);
    bad=require_service;bad.capabilities=4;assert(driver_protocol_begin(endpoint,&bad,&access)==-EOPNOTSUPP);
    bad=require_service;bad.id++;assert(driver_protocol_begin(endpoint,&bad,&access)==-EOPNOTSUPP);
    assert(!access.endpoint && !access.ops && !access.device && !access.info.id);
    const struct driver_protocol_descriptor late={.info={7,1,sizeof(service),0},.ops=&service};
    assert(device_add_protocol(dev,&late)==-EINVAL);
}
int main(int argc,char **argv)
{
    assert(argc==2);driver_core_init();
    if (!strcmp(argv[1],"native")) {
        device_t *dev=start();driver_endpoint_t *old=device_endpoint_get(dev);
        snapshot_and_access(dev,old,1);
        assert(!class_device_get_by_type(DEV_CLASS_CHAR,0));
        assert(!driver_unregister(&implementation));
        assert(class_device_binding_stopped(old));
        struct driver_protocol_access access;
        assert(driver_protocol_begin(old,&require_service,&access)==-ENODEV);
        generation=2;assert(!driver_register(&implementation));
        driver_endpoint_t *fresh=device_endpoint_get(dev);assert(fresh && fresh!=old);
        struct driver_protocol_info info;assert(!driver_endpoint_protocol_info(old,0,&info)&&info.version==1);
        assert(!driver_endpoint_protocol_info(fresh,0,&info)&&info.version==2);
        assert(driver_protocol_begin(fresh,&require_service,&access)==-EPROTONOSUPPORT);
        driver_endpoint_put(old);driver_endpoint_put(fresh);
        assert(!device_unregister(dev));device_put(dev);assert(released==1);
    } else if (!strcmp(argv[1],"classes")) {
        const net_dev_ops_t network={.send=net_send,.recv=net_recv,.mac=net_mac};
        implementation.class_type=DEV_CLASS_NET;implementation.class_ops=&network;
        device_t *dev=start();driver_endpoint_t *endpoint=device_endpoint_get(dev);
        snapshot_and_access(dev,endpoint,3);
        net_dev_stats_t counters;assert(!class_net_read_stats(endpoint,&counters));
        assert(counters.rx_packets==1 && counters.rx_drops==2 && counters.tx_packets==3 && counters.tx_drops==4);
        const struct driver_protocol_requirement need={DRIVER_PROTOCOL_NET,1,sizeof(network),0};
        struct driver_protocol_access access;assert(!driver_protocol_begin(endpoint,&need,&access));
        assert(((const net_dev_ops_t *)access.ops)->send(access.device,"a",1)==1);
        driver_protocol_end(&access);
        assert(!device_unregister(dev));
        assert(class_net_read_stats(endpoint,&counters)==-ENODEV && !counters.rx_packets);
        driver_endpoint_put(endpoint);device_put(dev);assert(released==1);
    } else if (!strcmp(argv[1],"drain")) {
        device_t *dev=start();driver_endpoint_t *endpoint=device_endpoint_get(dev);
        struct driver_protocol_access held,tmp;
        assert(!driver_protocol_begin(endpoint,&require_service,&held));
        pthread_t thread;assert(!pthread_create(&thread,NULL,unregister_thread,&implementation));
        for (;;) {
            int result=driver_protocol_begin(endpoint,&require_service,&tmp);
            if(result==-ENODEV)break;
            assert(!result);driver_protocol_end(&tmp);sched_yield();
        }
        assert(!__atomic_load_n(&removed,__ATOMIC_ACQUIRE));
        driver_protocol_end(&held);assert(!pthread_join(thread,NULL));
        assert(removed==1 && class_device_binding_stopped(endpoint));
        assert(!device_unregister(dev));device_put(dev);assert(!released);
        driver_endpoint_put(endpoint);assert(released==1);
    } else if (!strcmp(argv[1],"failure")) {
        mode=1;device_t *dev=start();
        assert(!device_endpoint_get(dev) && !dev->drv && !dev->resource_actions && dev->probe_error==-ENOMEM);
        assert(!driver_unregister(&implementation));mode=2;remove_failure=1;
        assert(!driver_register(&implementation));
        assert(dev->drv==&implementation && dev->resource_actions && dev->probe_error==-EIO);
        assert(!device_endpoint_get(dev));
        assert(driver_unregister(&implementation)==-ETIMEDOUT && dev->resource_actions);
        remove_failure=0;assert(!driver_unregister(&implementation) && !dev->resource_actions);
        assert(!device_unregister(dev));device_put(dev);assert(released==1);
    } else if (!strcmp(argv[1],"execution")) {
        mode=3;execution_failure=1;
        device_t *dev=start();driver_endpoint_t *endpoint=device_endpoint_get(dev);
        struct driver_protocol_access held,tmp;
        assert(!driver_protocol_begin(endpoint,&require_service,&held));execution_held=&held;
        /* Same task owns the active call: entering call drain here deadlocks.
         * Fallible execution stop must return before that drain is attempted. */
        assert(driver_unregister(&implementation)==-ETIMEDOUT);
        assert(stops[0]==1&&stops[1]==1&&!removed&&!releases&&dev->drv==&implementation);
        assert(dev->state==DEV_STATE_PREPARING&&held.ops);
        assert(driver_protocol_begin(endpoint,&require_service,&tmp)==-ENODEV);
        execution_failure=0;assert(!driver_unregister(&implementation));
        assert(stops[0]==2&&stops[1]==1&&removed==1&&releases==2&&!held.ops);
        driver_endpoint_put(endpoint);assert(!device_unregister(dev));device_put(dev);assert(released==1);
    } else if (!strcmp(argv[1],"execution-probe")) {
        mode=4;execution_failure=1;
        device_t *dev=start();
        assert(dev->state==DEV_STATE_PROBE_CLEANUP&&dev->drv==&implementation&&dev->probe_error==-EIO);
        assert(stops[0]==1&&stops[1]==1&&!removed&&!releases);
        execution_failure=0;assert(!driver_unregister(&implementation));
        assert(stops[0]==2&&stops[1]==1&&releases==2&&removed==1);
        mode=5;remove_failure=1;assert(!driver_register(&implementation));
        assert(dev->drv==&implementation&&dev->probe_error==-EIO&&!dev->class_dev);
        remove_failure=0;assert(!driver_unregister(&implementation));
        assert(!device_unregister(dev));device_put(dev);assert(released==1);
    } else assert(!"unknown workflow");
    puts("DRIVER_PROTOCOL: PASS workflow");return 0;
}
