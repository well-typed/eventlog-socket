#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "./write_buffer.h"

HIDDEN void es_write_buffer_push(WriteBuffer *wb, size_t size,
                                 uint8_t data[size]) {
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
}

HIDDEN void es_write_buffer_pop(WriteBuffer *wb) {
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

HIDDEN void es_write_buffer_free(WriteBuffer *wb) {
  // not the most efficient implementation,
  // but should be obviously correct.
  while (wb->head) {
    es_write_buffer_pop(wb);
  }
}
