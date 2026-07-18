# STM32 dashboard archive

This directory preserves the reusable, hardware-independent parts of the
former STM32 dashboard: the home/diagnostic models, renderer, Live2D state/loader,
and optional CJK font loader.

It is intentionally excluded from the kernel build. The original smart-home
controller, cloud protocol/configuration, Bluetooth command language and
self-tests were product-specific course application code and were removed
rather than kept in the platform layer.

The archived sources still document their original kernel dependencies. Treat
them as reference material for a future user-space dashboard port, not as a
supported standalone program.
