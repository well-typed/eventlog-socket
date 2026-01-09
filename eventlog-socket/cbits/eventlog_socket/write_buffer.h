#ifndef EVENTLOG_SOCKET_WRITE_BUFFER_H
#define EVENTLOG_SOCKET_WRITE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct write_buffer write_buffer_t;
typedef struct write_buffer_item write_buffer_item_t;

// invariant: head and last are both NULL or both not NULL.
struct write_buffer {
  write_buffer_item_t *head;
  write_buffer_item_t *last;
};

struct write_buffer_item {
  uint8_t *orig; // original data pointer (which we free)
  uint8_t *data;
  size_t size; // invariant: size is not zero
  write_buffer_item_t *next;
};

// push to the back.
// Caller must serialize externally (writer_write/write_iteration hold mutex)
// so that head/last invariants stay intact.
void __attribute__((visibility("hidden")))
write_buffer_push(write_buffer_t *wb, uint8_t *data, size_t size);

// pop from the front.
// Requires the same external synchronization as write_buffer_push.
void __attribute__((visibility("hidden"))) write_buffer_pop(write_buffer_t *wb);

// buf itself is not freed.
// it's safe to call write_buffer_free multiple times on the same buf.
void __attribute__((visibility("hidden")))
write_buffer_free(write_buffer_t *wb);

#endif /* EVENTLOG_SOCKET_WRITE_BUFFER_H */
