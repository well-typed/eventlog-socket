#ifndef EVENTLOG_SOCKET_WRITE_BUFFER_H
#define EVENTLOG_SOCKET_WRITE_BUFFER_H

#include "./macros.h"

#include <stddef.h>
#include <stdint.h>

typedef struct WriteBuffer WriteBuffer;
typedef struct WriteBufferItem WriteBufferItem;

// invariant: head and last are both NULL or both not NULL.
struct WriteBuffer {
  WriteBufferItem *head;
  WriteBufferItem *last;
};

struct WriteBufferItem {
  uint8_t *orig; // original data pointer (which we free)
  uint8_t *data;
  size_t size; // invariant: size is not zero
  WriteBufferItem *next;
};

// push to the back.
// Caller must serialize externally (writer_write/write_iteration hold mutex)
// so that head/last invariants stay intact.
void HIDDEN write_buffer_push(WriteBuffer *wb, uint8_t *data, size_t size);

// pop from the front.
// Requires the same external synchronization as write_buffer_push.
void HIDDEN write_buffer_pop(WriteBuffer *wb);

// buf itself is not freed.
// it's safe to call write_buffer_free multiple times on the same buf.
void HIDDEN write_buffer_free(WriteBuffer *wb);

#endif /* EVENTLOG_SOCKET_WRITE_BUFFER_H */
