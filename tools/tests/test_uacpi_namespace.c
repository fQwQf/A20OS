#include "firmware/acpi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char ram[65536];
static const void *map(void *o,uint64_t p,size_t n) {
    (void)o;return p<sizeof(ram)&&n<=sizeof(ram)-p?ram+p:NULL;
}
static void unmap(void *o,const void*p,size_t n) { (void)o;(void)p;(void)n; }
static fw_node_t find(fw_node_t root,const char *id) {
    fw_node_t c;
    int rc=root.ops->next_child(&root,NULL,&c);
    while(!rc) {
        if(c.ops->match_id(&c,id))return c;
        fw_node_t sub=find(c,id);if(sub.ops)return sub;
        fw_node_t prev=c;rc=root.ops->next_child(&root,&prev,&c);
    }
    return (fw_node_t){0};
}
int main(int argc,char **argv) {
    assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);
    assert(fread(ram,1,sizeof(ram),f)==sizeof(ram));fclose(f);
    acpi_context_t context;acpi_mapper_t mapper={.map=map,.unmap=unmap};
    assert(acpi_context_open(&context,&mapper,0x100)==0);
    fw_node_t root;assert(acpi_namespace_initialize(&context,&root)==0);
    fw_node_t dev=find(root,"A20T0001");assert(dev.ops);
    assert(dev.ops->available(&dev)==1);
    assert(dev.ops->match_id(&dev,"A20C0002"));
    fw_property_t prop;assert(dev.ops->property(&dev,"acpi,sta",&prop)==0);
    assert(prop.size==4&&*(const uint32_t*)prop.data==15);
    fw_resource_t resource;assert(dev.ops->resource(&dev,0,&resource)==0);
    assert(resource.type==FW_RESOURCE_MEMORY&&resource.address==0x10000000&&resource.size==0x1000);
    fw_reference_t irq;assert(dev.ops->irq(&dev,0,&irq)==0);
    assert(irq.count==2&&irq.args[0]==21&&irq.args[1]==8);
    assert(irq.node.context==root.context&&irq.node.id==root.id);
    fw_node_t tpm=find(root,"MSFT0101");assert(tpm.ops);
    assert(tpm.ops->property(&tpm,"acpi,tpm2-start-method",&prop)==0);
    assert(prop.size==4&&*(const uint32_t*)prop.data==6);
    assert(dev.ops->property(&dev,"acpi,tpm2-start-method",&prop)<0);
    fw_node_t parent=find(root,"A20P0001"), child=find(root,"A20C0003");
    assert(parent.ops&&child.ops&&parent.ops->available(&parent)==0);
    assert(parent.ops->children_available(&parent)==1&&child.ops->available(&child)==1);
    fw_node_t actual_parent;assert(child.ops->parent(&child,&actual_parent)==0);
    assert(actual_parent.context==parent.context);
    fw_node_t touchpad=find(root,"SYNA2BA6");assert(touchpad.ops);
    assert(touchpad.ops->property(&touchpad,"acpi,i2c-address",&prop)==0);
    assert(prop.size==4&&*(const uint32_t*)prop.data==0x2c);
    assert(touchpad.ops->property(&touchpad,"acpi,i2c-speed",&prop)==0);
    assert(prop.size==4&&*(const uint32_t*)prop.data==400000);
    assert(touchpad.ops->property(&touchpad,"acpi,hid-descriptor-register",&prop)==0);
    assert(prop.size==4&&*(const uint32_t*)prop.data==0x20);
    fw_reference_t attention;
    assert(touchpad.ops->reference(&touchpad,"acpi,gpio-attention","#gpio-cells",0,&attention)==0);
    assert(attention.count==2 && attention.args[0]==9 && attention.args[1]==1);
    assert(attention.node.ops->match_id(&attention.node,"AMDI0030")==1);
    fw_node_t pci;assert(acpi_pci_root(0,0,&pci)==0);
    uint32_t gsi;assert(acpi_pci_route(&pci,0,0,1,&gsi)==0&&gsi==21);
    assert(acpi_pci_route(&pci,1,0,1,&gsi)<0);
    puts("uACPI real AML namespace/HID/CID/STA/CRS/PRT PASS");return 0;
}
