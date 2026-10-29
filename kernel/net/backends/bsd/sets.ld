/* The BSD image is statically linked. It has no separate dynamic ELF loader.
 * Its real SYSINIT/module/sysctl/per-CPU pointer sets must survive section GC. */
SECTIONS
{
  .a20_bsd_sets : ALIGN(64)
  {
    a20_bsd___start_set_pcpu = .;
    KEEP(*(set_pcpu))
    a20_bsd___stop_set_pcpu = .;
    . = ALIGN(8);
    a20_bsd___start_set_sysinit_set = .;
    KEEP(*(set_sysinit_set))
    a20_bsd___stop_set_sysinit_set = .;
    a20_bsd___start_set_sysuninit_set = .;
    KEEP(*(set_sysuninit_set))
    a20_bsd___stop_set_sysuninit_set = .;
    a20_bsd___start_set_sysctl_set = .;
    KEEP(*(set_sysctl_set))
    a20_bsd___stop_set_sysctl_set = .;
    a20_bsd___start_set_modmetadata_set = .;
    KEEP(*(set_modmetadata_set))
    a20_bsd___stop_set_modmetadata_set = .;
  }
}
INSERT AFTER .data;
