#ifndef A20_METRICS_CONFIG_H
#define A20_METRICS_CONFIG_H
#ifndef CONFIG_A20_METRICS
#ifdef CONFIG_MCU
#define CONFIG_A20_METRICS 0
#else
#define CONFIG_A20_METRICS 1
#endif
#endif
#endif
