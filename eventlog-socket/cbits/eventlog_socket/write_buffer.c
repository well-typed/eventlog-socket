#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "./debug.h"
#include "./write_buffer.h"

void HIDDEN write_buffer_push(WriteBuffer *wb, uint8_t *data, size_t size) {
  DEBUG_TRACE("%p, %lu\n", (void *)data, size);
  uint8_t *copy = malloc(size);
  memcpy(copy, data, size);

  WriteBufferItem *item = malloc(sizeof(WriteBufferItem));
  item->orig = copy;
  item->data = copy;
  item->size = size;
  item->next = NULL;

  WriteBufferItem *last = wb->last;
  if (last == NULL) {
    assert(wb->head == NULL);

    wb->head = item;
    wb->last = item;
  } else {
    last->next = item;
    wb->last = item;
  }

  DEBUG_TRACE("%p %p\n", (void *)wb, (void *)wb->head);
}

void HIDDEN write_buffer_pop(WriteBuffer *wb) {
  WriteBufferItem *head = wb->head;
  if (head == NULL) {
    // buffer is empty: nothing to do.
    return;
  } else {
    wb->head = head->next;
    if (wb->last == head) {
      wb->last = NULL;
    }
    free(head->orig);
    free(head);
  }
}

void HIDDEN write_buffer_free(WriteBuffer *wb) {
  // not the most efficient implementation,
  // but should be obviously correct.
  while (wb->head) {
    write_buffer_pop(wb);
  }
}
