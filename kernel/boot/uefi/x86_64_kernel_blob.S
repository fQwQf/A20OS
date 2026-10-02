    .section .kernel, "a", %progbits
    .balign 16
    .global _binary_kernel_bin_start
_binary_kernel_bin_start:
    .incbin KERNEL_BIN_PATH
    .global _binary_kernel_bin_end
_binary_kernel_bin_end:
