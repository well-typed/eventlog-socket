// For POLLRDHUP
#define _GNU_SOURCE

#include <assert.h>
#include <stdbool.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <fcntl.h>

#include <Rts.h>

#include "eventlog_socket.h"

#define LISTEN_BACKLOG 5
#define POLL_LISTEN_TIMEOUT 10000
#define POLL_WRITE_TIMEOUT 1000

#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
#endif

// logging helper macros:
// - use PRINT_ERR to unconditionally log erroneous situations
// - otherwise use DEBUG_ERR
#define PRINT_ERR(...) fprintf(stderr, "ghc-eventlog-socket: " __VA_ARGS__)
#ifdef NDEBUG
#define DEBUG_ERR(fmt, ...)
#define DEBUG0_ERR(fmt)
#else
#define DEBUG_ERR(fmt, ...) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__, __VA_ARGS__)
#define DEBUG0_ERR(fmt) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__)
#endif

/*********************************************************************************
 * data definitions
 *********************************************************************************/


struct write_buffer_item {
  uint8_t *orig; // original data pointer (which we free)
  uint8_t *data;
  size_t size; // invariant: size is not zero
  struct write_buffer_item *next;
};

// invariant: head and last are both NULL or both not NULL.
struct write_buffer {
  struct write_buffer_item *head;
  struct write_buffer_item *last;
};

/*********************************************************************************
 * globals
 *********************************************************************************/

/* This module is concurrent.
 * There are two thread(group)s:
 * 1. RTS
 * 2. worker spawned by open_socket
 */

// variables read and written by worker only:
static bool initialized = false;
static int listen_fd = -1;
static const char *g_sock_path = NULL;

static void cleanup_socket(void) {
    if (g_sock_path) unlink(g_sock_path);
}


// concurrency variables
static pthread_t listen_thread;
static pthread_cond_t new_conn_cond;
static pthread_mutex_t mutex;

// variables accessed by both threads.
// their access should be guarded by mutex.
//
// Note: RTS writes client_fd in writer_stop.
static volatile int client_fd = -1;
static struct write_buffer wt = {
  .head = NULL,
  .last = NULL,
};

/*********************************************************************************
 * write_buffer
 *********************************************************************************/

// push to the back.
void write_buffer_push(struct write_buffer *buf, uint8_t *data, size_t size) {
  DEBUG_ERR("%p, %lu\n", data, size);
  uint8_t *copy = malloc(size);
  memcpy(copy, data, size);

  struct write_buffer_item *item = malloc(sizeof(struct write_buffer_item));
  item->orig = copy;
  item->data = copy;
  item->size = size;
  item->next = NULL;

  struct write_buffer_item *last = buf->last;
  if (last == NULL) {
    assert(buf->head == NULL);

    buf->head = item;
    buf->last = item;
  } else {
    last->next = item;
    buf->last = item;
  }

  DEBUG_ERR("%p %p %p\n", buf, &wt, buf->head);
};

// pop from the front.
void write_buffer_pop(struct write_buffer *buf) {
  struct write_buffer_item *head = buf->head;
  if (head == NULL) {
    // buffer is empty: nothing to do.
    return;
  } else {
    buf->head = head->next;
    if (buf->last == head) {
      buf->last = NULL;
    }
    free(head->orig);
    free(head);
  }
}

// buf itself is not freed.
// it's safe to call write_buffer_free multiple times on the same buf.
void write_buffer_free(struct write_buffer *buf) {
  // not the most effecient implementation,
  // but should be obviously correct.
  while (buf->head) {
    write_buffer_pop(buf);
  }
}

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void)
{
  // no-op
}

static void writer_enqueue(uint8_t *data, size_t size) {
  DEBUG_ERR("size: %p %lu\n", data, size);

  // TODO: check the size of the queue
  // if it's too big, we can start dropping blocks.

  // for now, we just push everythinb to the back of the buffer.
  write_buffer_push(&wt, data, size);

  DEBUG_ERR("wt.head = %p\n", wt.head);
}

static bool writer_write(void *eventlog, size_t size)
{
  DEBUG_ERR("size: %lu\n", size);
  pthread_mutex_lock(&mutex);
  int fd = client_fd;
  if (fd < 0) {
    goto exit;
  }

  DEBUG_ERR("client_fd = %d; wt.head = %p\n", fd, wt.head);

  if (wt.head != NULL) {
    // if there is stuff in queue already, we enqueue the current block.
    writer_enqueue(eventlog, size);
  } else {

    // and if there isn't, we can write immediately.
    int ret = write(fd, eventlog, size);
    DEBUG_ERR("write return %d\n", ret);

    if (ret == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // couldn't write anything, enqueue whole block
        writer_enqueue(eventlog, size);
        goto exit;
      } else if (errno == EPIPE) {
        // connection closed, simply exit
        goto exit;

      } else {
        PRINT_ERR("failed to write: %s\n", strerror(errno));
        goto exit;
      }
    } else {
      // we wrote something
      if (ret >= size) {
        // we wrote everything, nothing to do
        goto exit;
      } else {
        // we wrote only part of the buffer
        writer_enqueue(eventlog + ret, size - ret);
      }
    }
  }

exit:
  pthread_mutex_unlock(&mutex);
  return true;
}

static void writer_flush(void)
{
  // no-op
}

static void writer_stop(void)
{
  pthread_mutex_lock(&mutex);
  if (client_fd >= 0) {
    close(client_fd);
    client_fd = -1;
    write_buffer_free(&wt);
  }
  pthread_mutex_unlock(&mutex);
}

static const EventLogWriter SocketEventLogWriter = {
  .initEventLogWriter = writer_init,
  .writeEventLog = writer_write,
  .flushEventLog = writer_flush,
  .stopEventLogWriter = writer_stop
};

/*********************************************************************************
 * Main worker (in own thread)
 *********************************************************************************/

static void listen_iteration() {
  DEBUG0_ERR("enter");

  if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
    PRINT_ERR("listen() failed: %s\n", strerror(errno));
    abort();
  }

  struct sockaddr_un remote;
  unsigned int len;

  struct pollfd pfd_accept = {
    .fd = listen_fd,
    .events = POLLIN,
    .revents = 0,
  };

  // poll until we can accept
  while (true) {
    int ret = poll(&pfd_accept, 1, POLL_LISTEN_TIMEOUT);
    if (ret ==  -1) {
      PRINT_ERR("poll() failed: %s\n", strerror(errno));
      return;
    } else if (ret == 0) {
      DEBUG0_ERR("accept poll timed out\n");
    } else {
      // got connection
      break;
    }
  }

  // accept
  int fd = accept(listen_fd, (struct sockaddr *) &remote, &len);
  if (fd == -1) {
    PRINT_ERR("accept failed: %s\n", strerror(errno));
  }

  // set socket into non-blocking mode
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    PRINT_ERR("fnctl F_GETFL failed: %s\n", strerror(errno));
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    PRINT_ERR("fnctl F_SETFL failed: %s\n", strerror(errno));
  }

  // we stop existing logging
  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
  }

  // we got client_id now.
  pthread_mutex_lock(&mutex);
  client_fd = fd;
  // Drop lock to allow initial batch of events to be written.
  pthread_mutex_unlock(&mutex);

  // start writing
  startEventLogging(&SocketEventLogWriter);

  // Announce new connection
  pthread_cond_broadcast(&new_conn_cond);

  // we are done.
}

// nothing to write iteration.
//
// we poll only for whether the connection is closed.
static void nonwrite_iteration(int fd) {
  DEBUG_ERR("(%d)\n", fd);

  // Wait for socket to disconnect
  struct pollfd pfd = {
    .fd = fd,
    .events = POLLRDHUP,
    .revents = 0,
  };

  int ret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (ret == -1 && errno != EAGAIN) {
    // error
    PRINT_ERR("poll() failed: %s\n", strerror(errno));
    return;
  } else if (ret == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfd.revents | POLLRDHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

    // reset client_fd
    pthread_mutex_lock(&mutex);
    // note: writer_stop may close the connection as well.
    client_fd = -1;
    write_buffer_free(&wt);
    pthread_mutex_unlock(&mutex);
    return;
  }

  // we don't stop logging,
  // write function will be no-op with negative client_fd
  //
  // Before setting new client_fd we will stop the logging,
  // and restart if afterwards, so the header is written
  // to the new connection.
}

// write iteration.
//
// we poll for both: can we write, and whether the connection is closed.
static void write_iteration(int fd) {
  DEBUG_ERR("(%d)\n", fd);

  // Wait for socket to disconnect
  struct pollfd pfd = {
    .fd = fd,
    .events = POLLOUT | POLLRDHUP,
    .revents = 0,
  };

  int ret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (ret == -1 && errno != EAGAIN) {
    // error
    PRINT_ERR("poll() failed: %s\n", strerror(errno));
    return;
  } else if (ret == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfd.revents & POLLHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

    // reset client_fd
    pthread_mutex_lock(&mutex);
    assert(fd == client_fd);
    client_fd = -1;
    write_buffer_free(&wt);
    pthread_mutex_unlock(&mutex);
    return;
  }

  if (pfd.revents & POLLOUT) {
    DEBUG_ERR("(%d) POLLOUT\n", fd);

    pthread_mutex_lock(&mutex);
    while (wt.head) {
      struct write_buffer_item *item = wt.head;
      ret = write(client_fd, item->data, item->size);

      if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // couldn't write anything, shouldn't happend.
          // do nothing.
        } else if (errno == EPIPE) {
          client_fd = -1;
          write_buffer_free(&wt);
        } else {
          PRINT_ERR("failed to write: %s\n", strerror(errno));
        }

        // break out of the loop
        break;

      } else {
        // we wrote something
        if (ret >= item->size) {
          // we wrote whole element, try to write next element too
          write_buffer_pop(&wt);
          continue;
        } else {
          item->size -= ret;
          item->data += ret;
          break;
        }
      }
    }
    pthread_mutex_unlock(&mutex);
  }
}

static void iteration() {
  pthread_mutex_lock(&mutex);
  int fd = client_fd;
  bool empty = wt.head == NULL;
  DEBUG_ERR("fd = %d, wt.head = %p\n", fd, wt.head);
  pthread_mutex_unlock(&mutex);

  if (fd != -1) {
    if (empty) {
      nonwrite_iteration(fd);
    } else {
      write_iteration(fd);
    }
  } else {
    listen_iteration();
  }
}

/* Main loop of eventlog-socket own thread:
 * Currently it is two states:
 * - either we have connection, then we poll for writes (and drop of connection).
 * - or we don't have, then we poll for accept.
 */
static void *worker(void * _unused)
{
  while (true) {
    iteration();
  }

  return NULL; // unreachable
}

/*********************************************************************************
 * Initialization
 *********************************************************************************/

static void open_socket(const char *sock_path)
{
  DEBUG_ERR("enter: %s\n", sock_path);

  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = strdup(sock_path);

  struct sockaddr_un local;
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, sock_path, sizeof(local.sun_path) - 1);
  unlink(sock_path);
  if (bind(listen_fd, (struct sockaddr *) &local,
           sizeof(struct sockaddr_un)) == -1) {
    PRINT_ERR("failed to bind socket %s: %s\n", sock_path, strerror(errno));
    abort();
  }

  int ret = pthread_create(&listen_thread, NULL, worker, NULL);
  if (ret != 0) {
    PRINT_ERR("failed to spawn thread: %s\n", strerror(ret));
    abort();
  }
}


/*********************************************************************************
 * Public interface
 *********************************************************************************/


void eventlog_socket_init(const char *sock_path)
{
  if (!initialized) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&new_conn_cond, NULL);
    atexit(cleanup_socket);
    initialized = true;
  }

  if (!sock_path)
    return;

  open_socket(sock_path);
}

void eventlog_socket_wait(void)
{
  pthread_mutex_lock(&mutex);
  while (client_fd == -1) {
    int ret = pthread_cond_wait(&new_conn_cond, &mutex);
    if (ret != 0) {
      PRINT_ERR("failed to wait on condition variable: %s\n", strerror(ret));
    }
  }
  pthread_mutex_unlock(&mutex);
}

void eventlog_socket_start(const char *sock_path, bool wait)
{
  if (!initialized) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&new_conn_cond, NULL);
    initialized = true;
  }

  if (!sock_path)
    return;

  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    PRINT_ERR("eventlog is not supported.\n");
    return;
  }

  // we stop existing logging
  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
  }

  // ... and restart with outer socket writer,
  // which is no-op so far.
  //
  // This trickery is to avoid
  //
  //     printAndClearEventLog: could not flush event log
  //
  // warning messages from showing up in stderr.
  startEventLogging(&SocketEventLogWriter);

  open_socket(sock_path);
  if (wait) {
    DEBUG_ERR("ghc-eventlog-socket: Waiting for connection to %s...\n", sock_path);
    eventlog_socket_wait();
  }
}

