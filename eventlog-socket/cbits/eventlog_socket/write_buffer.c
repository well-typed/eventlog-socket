#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "./debug.h"
#include "./write_buffer.h"

void __attribute__((visibility("hidden")))
write_buffer_push(write_buffer_t *wb, uint8_t *data, size_t size) {
  DEBUG_TRACE("%p, %lu\n", data, size);
  uint8_t *copy = malloc(size);
  memcpy(copy, data, size);

  write_buffer_item_t *item = malloc(sizeof(write_buffer_item_t));
  item->orig = copy;
  item->data = copy;
  item->size = size;
  item->next = NULL;

  write_buffer_item_t *last = wb->last;
  if (last == NULL) {
    assert(wb->head == NULL);

    wb->head = item;
    wb->last = item;
  } else {
    last->next = item;
    wb->last = item;
  }

  DEBUG_TRACE("%p %p\n", wb, wb->head);
}

void __attribute__((visibility("hidden")))
write_buffer_pop(write_buffer_t *wb) {
  write_buffer_item_t *head = wb->head;
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

void __attribute__((visibility("hidden")))
write_buffer_free(write_buffer_t *wb) {
  // not the most efficient implementation,
  // but should be obviously correct.
  while (wb->head) {
    write_buffer_pop(wb);
  }
}
