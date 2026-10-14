#ifndef _IPC_ENVELOPE_ACQUIRE_RACE_TEST_H
#define _IPC_ENVELOPE_ACQUIRE_RACE_TEST_H

#include <stdint.h>

#define A20_ENVELOPE_RACE_OP_SCM   1
#define A20_ENVELOPE_RACE_OP_PIDFD 2

#define A20_ENVELOPE_RACE_CMD_ARM    1
#define A20_ENVELOPE_RACE_CMD_RESULT 2
#define A20_ENVELOPE_RACE_CMD_CANCEL 3

struct task_t;

typedef struct a20_envelope_race_test_result {
    int32_t operation;
    int32_t status;
    int32_t installed_fd;
    int32_t pin_release_count;
} a20_envelope_race_test_result_t;

#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
int a20_envelope_race_test_arm(int operation, int replacement_fd);
int a20_envelope_race_test_result(a20_envelope_race_test_result_t *out);
void a20_envelope_race_test_cancel_task(struct task_t *task);
int a20_envelope_race_test_before_mediation(int operation, int installed_fd,
                                             void *incoming_vfile);
void a20_envelope_race_test_note_pin_release(int operation,
                                              uint64_t incoming_identity);
#endif

#endif
