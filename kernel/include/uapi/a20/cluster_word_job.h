#ifndef A20_CLUSTER_WORD_JOB_H
#define A20_CLUSTER_WORD_JOB_H
#include "cluster_job.h"
#include "cluster_words.h"
#define A20_JOB_WORDCOUNT 2u
#define A20_JOB_MAX_TEXT 1024u
#define A20_JOB_WORD_LEAVES 16u
#define A20_JOB_WORD_TASKS 31u
#define A20_JOB_WORD_CHUNK 76u
/* Split only between words; punctuation remains in a shard and is a delimiter.
 * The coordinator frames committed dictionaries, never computes word counts. */
static inline int
a20_job_word_graph(a20_job_task_t *tasks, uint32_t capacity,
                   uint8_t inputs[A20_JOB_WORD_LEAVES][A20_JOB_WORD_CHUNK],
                   uint8_t outputs[A20_JOB_WORD_TASKS][A20_WORD_RESULT],
                   const uint8_t *text, uint32_t len) {
    if (!tasks || !inputs || !outputs || (len && !text) ||
        len > A20_JOB_MAX_TEXT || capacity < A20_JOB_WORD_TASKS)
        return -1;
    unsigned offset = 0, leaves = 0;
    do {
        if (leaves == A20_JOB_WORD_LEAVES)
            return -1;
        unsigned end = len - offset > A20_JOB_WORD_CHUNK
                           ? offset + A20_JOB_WORD_CHUNK
                           : len;
        if (end < len && a20_word_char(text[end]) &&
            a20_word_char(text[end - 1])) {
            while (end > offset && a20_word_char(text[end - 1]))
                end--;
            if (end == offset)
                return -1;
        }
        for (unsigned i = offset; i < end; i++)
            inputs[leaves][i - offset] = text[i];
        tasks[leaves] = (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                                         .operator_id = A20_WORK_WORD_MAP,
                                         .flags = A20_WORK_REPLAY_SAFE,
                                         .input = inputs[leaves],
                                         .input_len = end - offset,
                                         .output = outputs[leaves],
                                         .output_capacity = A20_WORD_RESULT};
        leaves++;
        offset = end;
    } while (offset < len);
    unsigned level[A20_JOB_WORD_LEAVES], width = leaves, next = leaves;
    for (unsigned i = 0; i < leaves; i++)
        level[i] = i;
    while (width > 1) {
        unsigned out = 0;
        for (unsigned i = 0; i < width; i += 2) {
            if (i + 1 == width) {
                level[out++] = level[i];
                continue;
            }
            tasks[next] = (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                                           .operator_id = A20_WORK_WORD_MERGE,
                                           .flags = A20_WORK_REPLAY_SAFE |
                                                    A20_WORK_ASSOCIATIVE,
                                           .children = {level[i], level[i + 1]},
                                           .child_count = 2,
                                           .output = outputs[next],
                                           .output_capacity = A20_WORD_RESULT,
                                           .combine = a20_job_concat_parts};
            level[out++] = next++;
        }
        width = out;
    }
    return next;
}
#endif
