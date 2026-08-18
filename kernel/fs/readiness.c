#include "fs/readiness.h"

#include "core/consts.h"
#include "core/poll.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"

#define READINESS_FALLBACK_TICKS \
    (MS_TO_TICKS(1) ? MS_TO_TICKS(1) : 1)

#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT)
#define LS2K_READY_MARK(ch) uart_putc(ch)
#else
#define LS2K_READY_MARK(ch) do { } while (0)
#endif

typedef struct readiness_slot {
    vfile_t *file;
    int gfd;
    uint64_t identity;
    readiness_source_t sources[READINESS_MAX_FILE_SOURCES];
    uint64_t source_generations[READINESS_MAX_FILE_SOURCES];
    size_t source_count;
} readiness_slot_t;

typedef struct readiness_link {
    readiness_source_t source;
    wait_queue_entry_t entry;
} readiness_link_t;

void readiness_state_init(readiness_state_t *state, uint32_t mode)
{
    if (!state)
        return;
    state->mode = mode;
    state->active = 0;
    state->generation = 0;
    state->enabled = true;
}

void readiness_state_rearm(readiness_state_t *state, uint32_t mode)
{
    readiness_state_init(state, mode);
}

static uint64_t readiness_slot_generation(const readiness_slot_t *slot)
{
    uint64_t generation = 0;
    for (size_t i = 0; i < slot->source_count; i++) {
        uintptr_t queue = (uintptr_t)slot->sources[i].queue;
        uint64_t value = wait_queue_generation(slot->sources[i].queue);
        generation ^= value + (uint64_t)(queue >> 4) +
                      0x9e3779b97f4a7c15ULL +
                      (generation << 6) + (generation >> 2);
    }
    return generation;
}

static bool readiness_collect_sources(readiness_slot_t *slot,
                                      readiness_interest_t *item)
{
    vfile_ops_t *ops = slot->file ? slot->file->ops : NULL;
    if (!ops || !ops->poll_sources)
        return true;

    readiness_source_t first[READINESS_MAX_FILE_SOURCES];
    slot->source_count = ops->poll_sources(
        slot->file, item->events, slot->sources,
        READINESS_MAX_FILE_SOURCES);
    if (slot->source_count > READINESS_MAX_FILE_SOURCES)
        slot->source_count = READINESS_MAX_FILE_SOURCES;
    size_t first_count = slot->source_count;
    memcpy(first, slot->sources, first_count * sizeof(*first));
    uint64_t first_generations[READINESS_MAX_FILE_SOURCES];
    for (size_t i = 0; i < first_count; i++)
        first_generations[i] = wait_queue_generation(first[i].queue);

    slot->source_count = ops->poll_sources(
        slot->file, item->events, slot->sources,
        READINESS_MAX_FILE_SOURCES);
    if (slot->source_count > READINESS_MAX_FILE_SOURCES)
        slot->source_count = READINESS_MAX_FILE_SOURCES;
    bool stable = first_count == slot->source_count;
    for (size_t i = 0; i < slot->source_count; i++) {
        slot->source_generations[i] =
            wait_queue_generation(slot->sources[i].queue);
        if (i >= first_count ||
            first[i].queue != slot->sources[i].queue ||
            first[i].key != slot->sources[i].key ||
            first[i].deadline != slot->sources[i].deadline ||
            first_generations[i] != slot->source_generations[i])
            stable = false;
    }
    return stable;
}

static bool readiness_sources_changed(const readiness_slot_t *slots,
                                      size_t count)
{
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < slots[i].source_count; j++)
            if (slots[i].source_generations[j] !=
                wait_queue_generation(slots[i].sources[j].queue))
                return true;
    }
    return false;
}

static short readiness_filter(short raw, uint64_t generation,
                              readiness_state_t *state)
{
    if (!state)
        return raw;
    if (!state->enabled)
        return 0;
    if (!raw) {
        state->active = 0;
        state->generation = generation;
        return 0;
    }

    bool deliver = !(state->mode & READINESS_MODE_EDGE) ||
                   !state->active || state->generation != generation;
    state->active = raw;
    state->generation = generation;
    if (!deliver)
        return 0;
    if (state->mode & READINESS_MODE_ONESHOT)
        state->enabled = false;
    return raw;
}

static int readiness_scan(readiness_interest_t *items,
                          readiness_slot_t *slots, size_t count,
                          size_t max_ready)
{
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        readiness_interest_t *item = &items[i];
        readiness_slot_t *slot = &slots[i];
        item->revents = 0;
        if (max_ready && (size_t)ready >= max_ready)
            continue;
        if (item->fd < 0)
            continue;
        if (!slot->file ||
            (!(item->flags & READINESS_F_GLOBAL_FD) &&
             !fdtable_current_matches_file(item->fd, slot->gfd,
                                           slot->identity)) ||
            (item->expected_identity &&
             item->expected_identity != slot->identity)) {
            item->revents = POLLNVAL;
            ready++;
            continue;
        }

        int result = vfs_poll_file(slot->file, item->events);
        short raw = result < 0 ? POLLNVAL : (short)result;
        raw &= (short)(item->events | POLLERR | POLLHUP | POLLNVAL);
        item->revents = readiness_filter(
            raw, readiness_slot_generation(slot), item->state);
        if (item->revents)
            ready++;
    }
    return ready;
}

static bool readiness_extra_ready(const readiness_extra_t *extra,
                                  size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (extra[i].ready && extra[i].ready(extra[i].arg))
            return true;
    return false;
}

static uint64_t readiness_min_deadline(uint64_t current, uint64_t candidate)
{
    if (!candidate)
        return current;
    return !current || candidate < current ? candidate : current;
}

int readiness_wait_once(readiness_interest_t *items, size_t count,
                        const readiness_extra_t *extra, size_t extra_count,
                        size_t max_ready, uint64_t deadline,
                        bool has_deadline)
{
    LS2K_READY_MARK('1');
    if ((!items && count) || (!extra && extra_count))
        return -EINVAL;
    if (count > (size_t)MAX_FILES * 3)
        return -EINVAL;

    readiness_slot_t *slots = count ? kcalloc(count, sizeof(*slots)) : NULL;
    size_t link_cap = count * READINESS_MAX_FILE_SOURCES + extra_count + 1;
    readiness_link_t *links = link_cap ?
        kcalloc(link_cap, sizeof(*links)) : NULL;
    if ((count && !slots) || (link_cap && !links)) {
        if (slots) kfree(slots);
        if (links) kfree(links);
        return -ENOMEM;
    }
    LS2K_READY_MARK('2');

    bool has_local = false;
    bool fallback = false;
    bool sources_stable = true;
    uint64_t park_deadline = has_deadline ? deadline : 0;
    for (size_t i = 0; i < count; i++) {
        readiness_interest_t *item = &items[i];
        readiness_slot_t *slot = &slots[i];
        item->revents = 0;
        if (item->fd < 0)
            continue;
        if (item->flags & READINESS_F_GLOBAL_FD) {
            slot->gfd = item->fd;
            slot->file = vfs_get_file_ref(item->fd);
        } else {
            has_local = true;
            slot->file = fdtable_get_current_file_ref(item->fd, &slot->gfd);
        }
        if (!slot->file)
            continue;
        slot->identity = slot->file->identity;
        if (!readiness_collect_sources(slot, item))
            sources_stable = false;
        if (!slot->source_count)
            fallback = true;
        for (size_t j = 0; j < slot->source_count; j++)
            park_deadline = readiness_min_deadline(
                park_deadline, slot->sources[j].deadline);
    }
    for (size_t i = 0; i < extra_count; i++)
        park_deadline = readiness_min_deadline(
            park_deadline, extra[i].source.deadline);
    LS2K_READY_MARK('3');

    int ready = readiness_scan(items, slots, count, max_ready);
    LS2K_READY_MARK('4');
    bool external_ready = readiness_extra_ready(extra, extra_count);
    bool sources_changed = !sources_stable ||
                           readiness_sources_changed(slots, count);
    if (external_ready || ready > 0 || sources_changed) {
        ready = external_ready || (ready == 0 && sources_changed) ?
                READINESS_RETRY : ready;
        goto out;
    }
    uint64_t now = timer_get_ticks();
    LS2K_READY_MARK('5');
    if (has_deadline && now >= deadline) {
        ready = 0;
        goto out;
    }
    if (fallback)
        park_deadline = readiness_min_deadline(
            park_deadline, now + READINESS_FALLBACK_TICKS);

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, park_deadline);
    if (!token.task) {
        ready = token.prepare_error == PROC_PARK_PREPARE_TIMEOUT_CAPACITY ?
                -EAGAIN : -EINTR;
        goto out;
    }

    size_t linked = 0;
    wait_queue_t *fdtable_waiters =
        has_local ? fdtable_current_readiness_queue() : NULL;
    if (fdtable_waiters) {
        links[linked].source.queue = fdtable_waiters;
        if (wait_queue_link(fdtable_waiters, &links[linked].entry,
                            token, 0))
            linked++;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < slots[i].source_count; j++) {
            readiness_source_t source = slots[i].sources[j];
            if (!source.queue)
                continue;
            links[linked].source = source;
            if (wait_queue_link(source.queue, &links[linked].entry,
                                token, source.key))
                linked++;
        }
    }
    for (size_t i = 0; i < extra_count; i++) {
        readiness_source_t source = extra[i].source;
        if (!source.queue)
            continue;
        links[linked].source = source;
        if (wait_queue_link(source.queue, &links[linked].entry,
                            token, source.key))
            linked++;
    }

    ready = readiness_scan(items, slots, count, max_ready);
    external_ready = readiness_extra_ready(extra, extra_count);
    sources_changed = readiness_sources_changed(slots, count);
    if (ready > 0 || external_ready || sources_changed ||
        (has_deadline && timer_get_ticks() >= deadline)) {
        (void)proc_park_cancel(token);
        if (external_ready || (ready == 0 && sources_changed))
            ready = READINESS_RETRY;
    } else {
        proc_wake_reason_t reason = proc_park_commit(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            ready = -EINTR;
        else
            ready = READINESS_RETRY;
    }

    for (size_t i = 0; i < linked; i++)
        wait_queue_unlink(links[i].source.queue, &links[i].entry);
    proc_park_finish(token);
out:
    LS2K_READY_MARK('6');
    for (size_t i = 0; i < count; i++)
        if (slots[i].file)
            vfs_put_file_ref(slots[i].gfd, slots[i].file);
    kfree(links);
    kfree(slots);
    return ready;
}
