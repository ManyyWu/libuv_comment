/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <limits.h> /* IOV_MAX */

#if defined(__APPLE__)
# include <sys/event.h>
# include <sys/time.h>
# include <sys/select.h>

/* Forward declaration */
typedef struct uv__stream_select_s uv__stream_select_t;

struct uv__stream_select_s {
  uv_stream_t* stream;
  uv_thread_t thread;
  uv_sem_t close_sem;
  uv_sem_t async_sem;
  uv_async_t async;
  int events;
  int fake_fd;
  int int_fd;
  int fd;
  fd_set* sread;
  size_t sread_sz;
  fd_set* swrite;
  size_t swrite_sz;
};

/* Due to a possible kernel bug at least in OS X 10.10 "Yosemite",
 * EPROTOTYPE can be returned while trying to write to a socket that is
 * shutting down. If we retry the write, we should get the expected EPIPE
 * instead.
 */
# define RETRY_ON_WRITE_ERROR(errno) (errno == EINTR || errno == EPROTOTYPE)
# define IS_TRANSIENT_WRITE_ERROR(errno, send_handle) \
    (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || \
     (errno == EMSGSIZE && send_handle != NULL))
#else
# define RETRY_ON_WRITE_ERROR(errno) (errno == EINTR)
# define IS_TRANSIENT_WRITE_ERROR(errno, send_handle) \
    (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
#endif /* defined(__APPLE__) */

static void uv__stream_connect(uv_stream_t*);
static void uv__write(uv_stream_t* stream);
static void uv__read(uv_stream_t* stream);
static void uv__stream_io(uv_loop_t* loop, uv__io_t* w, unsigned int events);
static void uv__write_callbacks(uv_stream_t* stream);
static size_t uv__write_req_size(uv_write_t* req);

/** 初始化流 **/
void uv__stream_init(uv_loop_t* loop,
                     uv_stream_t* stream,
                     uv_handle_type type) {
  int err;

  uv__handle_init(loop, (uv_handle_t*)stream, type); /** 将stream插入handle_queue **/
  stream->read_cb = NULL;
  stream->alloc_cb = NULL;
  stream->close_cb = NULL;
  stream->connection_cb = NULL;
  stream->connect_req = NULL;
  stream->shutdown_req = NULL;
  stream->accepted_fd = -1;
  stream->queued_fds = NULL;
  stream->delayed_error = 0;
  QUEUE_INIT(&stream->write_queue);
  QUEUE_INIT(&stream->write_completed_queue);
  stream->write_queue_size = 0;

  /** 创建备用文件描述符, uv__stream_init第一次调用和uv__stream_connect创建失败时起作用 **/
  if (loop->emfile_fd == -1) {
    err = uv__open_cloexec("/dev/null", O_RDONLY);
    if (err < 0)
        /* In the rare case that "/dev/null" isn't mounted open "/"
         * instead.
         */
        err = uv__open_cloexec("/", O_RDONLY);
    if (err >= 0)
      loop->emfile_fd = err;
  }

#if defined(__APPLE__)
  stream->select = NULL;
#endif /* defined(__APPLE_) */

  /** 初始化i/o观察者 **/
  uv__io_init(&stream->io_watcher, uv__stream_io, -1);
}


static void uv__stream_osx_interrupt_select(uv_stream_t* stream) {
#if defined(__APPLE__)
  /* Notify select() thread about state change */
  uv__stream_select_t* s;
  int r;

  s = stream->select;
  if (s == NULL)
    return;

  /* Interrupt select() loop
   * NOTE: fake_fd and int_fd are socketpair(), thus writing to one will
   * emit read event on other side
   */
  do
    r = write(s->fake_fd, "x", 1);
  while (r == -1 && errno == EINTR);

  assert(r == 1);
#else  /* !defined(__APPLE__) */
  /* No-op on any other platform */
#endif  /* !defined(__APPLE__) */
}


#if defined(__APPLE__)
static void uv__stream_osx_select(void* arg) {
  uv_stream_t* stream;
  uv__stream_select_t* s;
  char buf[1024];
  int events;
  int fd;
  int r;
  int max_fd;

  stream = arg;
  s = stream->select;
  fd = s->fd;

  if (fd > s->int_fd)
    max_fd = fd;
  else
    max_fd = s->int_fd;

  while (1) {
    /* Terminate on semaphore */
    if (uv_sem_trywait(&s->close_sem) == 0)
      break;

    /* Watch fd using select(2) */
    memset(s->sread, 0, s->sread_sz);
    memset(s->swrite, 0, s->swrite_sz);

    if (uv__io_active(&stream->io_watcher, POLLIN))
      FD_SET(fd, s->sread);
    if (uv__io_active(&stream->io_watcher, POLLOUT))
      FD_SET(fd, s->swrite);
    FD_SET(s->int_fd, s->sread);

    /* Wait indefinitely for fd events */
    r = select(max_fd + 1, s->sread, s->swrite, NULL, NULL);
    if (r == -1) {
      if (errno == EINTR)
        continue;

      /* XXX: Possible?! */
      abort();
    }

    /* Ignore timeouts */
    if (r == 0)
      continue;

    /* Empty socketpair's buffer in case of interruption */
    if (FD_ISSET(s->int_fd, s->sread))
      while (1) {
        r = read(s->int_fd, buf, sizeof(buf));

        if (r == sizeof(buf))
          continue;

        if (r != -1)
          break;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;

        if (errno == EINTR)
          continue;

        abort();
      }

    /* Handle events */
    events = 0;
    if (FD_ISSET(fd, s->sread))
      events |= POLLIN;
    if (FD_ISSET(fd, s->swrite))
      events |= POLLOUT;

    assert(events != 0 || FD_ISSET(s->int_fd, s->sread));
    if (events != 0) {
      ACCESS_ONCE(int, s->events) = events;

      uv_async_send(&s->async);
      uv_sem_wait(&s->async_sem);

      /* Should be processed at this stage */
      assert((s->events == 0) || (stream->flags & UV_HANDLE_CLOSING));
    }
  }
}


static void uv__stream_osx_select_cb(uv_async_t* handle) {
  uv__stream_select_t* s;
  uv_stream_t* stream;
  int events;

  s = container_of(handle, uv__stream_select_t, async);
  stream = s->stream;

  /* Get and reset stream's events */
  events = s->events;
  ACCESS_ONCE(int, s->events) = 0;

  assert(events != 0);
  assert(events == (events & (POLLIN | POLLOUT)));

  /* Invoke callback on event-loop */
  if ((events & POLLIN) && uv__io_active(&stream->io_watcher, POLLIN))
    uv__stream_io(stream->loop, &stream->io_watcher, POLLIN);

  if ((events & POLLOUT) && uv__io_active(&stream->io_watcher, POLLOUT))
    uv__stream_io(stream->loop, &stream->io_watcher, POLLOUT);

  if (stream->flags & UV_HANDLE_CLOSING)
    return;

  /* NOTE: It is important to do it here, otherwise `select()` might be called
   * before the actual `uv__read()`, leading to the blocking syscall
   */
  uv_sem_post(&s->async_sem);
}


static void uv__stream_osx_cb_close(uv_handle_t* async) {
  uv__stream_select_t* s;

  s = container_of(async, uv__stream_select_t, async);
  uv__free(s);
}


int uv__stream_try_select(uv_stream_t* stream, int* fd) {
  /*
   * kqueue doesn't work with some files from /dev mount on osx.
   * select(2) in separate thread for those fds
   */

  struct kevent filter[1];
  struct kevent events[1];
  struct timespec timeout;
  uv__stream_select_t* s;
  int fds[2];
  int err;
  int ret;
  int kq;
  int old_fd;
  int max_fd;
  size_t sread_sz;
  size_t swrite_sz;

  kq = kqueue();
  if (kq == -1) {
    perror("(libuv) kqueue()");
    return UV__ERR(errno);
  }

  EV_SET(&filter[0], *fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);

  /* Use small timeout, because we only want to capture EINVALs */
  timeout.tv_sec = 0;
  timeout.tv_nsec = 1;

  do
    ret = kevent(kq, filter, 1, events, 1, &timeout);
  while (ret == -1 && errno == EINTR);

  uv__close(kq);

  if (ret == -1)
    return UV__ERR(errno);

  if (ret == 0 || (events[0].flags & EV_ERROR) == 0 || events[0].data != EINVAL)
    return 0;

  /* At this point we definitely know that this fd won't work with kqueue */

  /*
   * Create fds for io watcher and to interrupt the select() loop.
   * NOTE: do it ahead of malloc below to allocate enough space for fd_sets
   */
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    return UV__ERR(errno);

  max_fd = *fd;
  if (fds[1] > max_fd)
    max_fd = fds[1];

  sread_sz = ROUND_UP(max_fd + 1, sizeof(uint32_t) * NBBY) / NBBY;
  swrite_sz = sread_sz;

  s = uv__malloc(sizeof(*s) + sread_sz + swrite_sz);
  if (s == NULL) {
    err = UV_ENOMEM;
    goto failed_malloc;
  }

  s->events = 0;
  s->fd = *fd;
  s->sread = (fd_set*) ((char*) s + sizeof(*s));
  s->sread_sz = sread_sz;
  s->swrite = (fd_set*) ((char*) s->sread + sread_sz);
  s->swrite_sz = swrite_sz;

  err = uv_async_init(stream->loop, &s->async, uv__stream_osx_select_cb);
  if (err)
    goto failed_async_init;

  s->async.flags |= UV_HANDLE_INTERNAL;
  uv__handle_unref(&s->async);

  err = uv_sem_init(&s->close_sem, 0);
  if (err != 0)
    goto failed_close_sem_init;

  err = uv_sem_init(&s->async_sem, 0);
  if (err != 0)
    goto failed_async_sem_init;

  s->fake_fd = fds[0];
  s->int_fd = fds[1];

  old_fd = *fd;
  s->stream = stream;
  stream->select = s;
  *fd = s->fake_fd;

  err = uv_thread_create(&s->thread, uv__stream_osx_select, stream);
  if (err != 0)
    goto failed_thread_create;

  return 0;

failed_thread_create:
  s->stream = NULL;
  stream->select = NULL;
  *fd = old_fd;

  uv_sem_destroy(&s->async_sem);

failed_async_sem_init:
  uv_sem_destroy(&s->close_sem);

failed_close_sem_init:
  uv__close(fds[0]);
  uv__close(fds[1]);
  uv_close((uv_handle_t*) &s->async, uv__stream_osx_cb_close);
  return err;

failed_async_init:
  uv__free(s);

failed_malloc:
  uv__close(fds[0]);
  uv__close(fds[1]);

  return err;
}
#endif /* defined(__APPLE__) */

/** 打开流, 将文件描述符关联到流, 关联之后才可以操作流 **/
int uv__stream_open(uv_stream_t* stream, int fd, int flags) {
#if defined(__APPLE__)
  int enable;
#endif

  /** 流已经打开 **/
  if (!(stream->io_watcher.fd == -1 || stream->io_watcher.fd == fd))
    return UV_EBUSY;

  /** 设置标志 **/
  assert(fd >= 0);
  stream->flags |= flags;

  if (stream->type == UV_TCP) {
    /** 禁用Nagle算法 **/
    if ((stream->flags & UV_HANDLE_TCP_NODELAY) && uv__tcp_nodelay(fd, 1))
      return UV__ERR(errno);

    /** 开启keep_alive **/
    /* TODO Use delay the user passed in. */
    if ((stream->flags & UV_HANDLE_TCP_KEEPALIVE) &&
        uv__tcp_keepalive(fd, 1, 60)) {
      return UV__ERR(errno);
    }
  }

#if defined(__APPLE__)
  enable = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &enable, sizeof(enable)) &&
      errno != ENOTSOCK &&
      errno != EINVAL) {
    return UV__ERR(errno);
  }
#endif

  /** fd保存在流中, 以供操作流时使用 **/
  stream->io_watcher.fd = fd;

  return 0;
}

/** 清空write_queue并为相关request设置错误码 **/
void uv__stream_flush_write_queue(uv_stream_t* stream, int error) {
  uv_write_t* req;
  QUEUE* q;
  while (!QUEUE_EMPTY(&stream->write_queue)) {
    q = QUEUE_HEAD(&stream->write_queue);
    QUEUE_REMOVE(q);

    req = QUEUE_DATA(q, uv_write_t, queue);
    req->error = error;

    QUEUE_INSERT_TAIL(&stream->write_completed_queue, &req->queue);
  }
}

/** 清除stream中的requests **/
void uv__stream_destroy(uv_stream_t* stream) {
  assert(!uv__io_active(&stream->io_watcher, POLLIN | POLLOUT));
  assert(stream->flags & UV_HANDLE_CLOSED);

  /** 取消connect_req **/
  if (stream->connect_req) {
    uv__req_unregister(stream->loop, stream->connect_req);
    stream->connect_req->cb(stream->connect_req, UV_ECANCELED);
    stream->connect_req = NULL;
  }

  /** 取消所有write_req, 即从write_queue移动到complete_queue **/
  uv__stream_flush_write_queue(stream, UV_ECANCELED);
  /** 遍历complete_queue, 回调返回错误码 **/
  uv__write_callbacks(stream);

  /** 取消shutdown_req **/
  if (stream->shutdown_req) {
    /* The ECANCELED error code is a lie, the shutdown(2) syscall is a
     * fait accompli at this point. Maybe we should revisit this in v0.11.
     * A possible reason for leaving it unchanged is that it informs the
     * callee that the handle has been destroyed.
     */
    uv__req_unregister(stream->loop, stream->shutdown_req);
    stream->shutdown_req->cb(stream->shutdown_req, UV_ECANCELED);
    stream->shutdown_req = NULL;
  }

  assert(stream->write_queue_size == 0);
}


/* Implements a best effort approach to mitigating accept() EMFILE errors.
 * We have a spare file descriptor stashed away that we close to get below
 * the EMFILE limit. Next, we accept all pending connections and close them
 * immediately to signal the clients that we're overloaded - and we are, but
 * we still keep on trucking.
 *
 * There is one caveat: it's not reliable in a multi-threaded environment.
 * The file descriptor limit is per process. Our party trick fails if another
 * thread opens a file or creates a socket in the time window between us
 * calling close() and accept().
 */
static int uv__emfile_trick(uv_loop_t* loop, int accept_fd) {
  int err;
  int emfile_fd;

  if (loop->emfile_fd == -1)
    return UV_EMFILE;

  /** 关闭emfile_fd **/
  uv__close(loop->emfile_fd);
  loop->emfile_fd = -1;

  /** accpet并关闭 **/
  do {
    err = uv__accept(accept_fd);
    if (err >= 0)
      uv__close(err);
  } while (err >= 0 || err == UV_EINTR);

  /** 创建emfile_fd, 未成功会在下次uv__stream_init尝试创建 **/
  emfile_fd = uv__open_cloexec("/", O_RDONLY);
  if (emfile_fd >= 0)
    loop->emfile_fd = emfile_fd;

  return err;
}


#if defined(UV_HAVE_KQUEUE)
# define UV_DEC_BACKLOG(w) w->rcount--;
#else
# define UV_DEC_BACKLOG(w) /* no-op */
#endif /* defined(UV_HAVE_KQUEUE) */


void uv__server_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  uv_stream_t* stream;
  int err;

  stream = container_of(w, uv_stream_t, io_watcher);
  assert(events & POLLIN);
  assert(stream->accepted_fd == -1);
  assert(!(stream->flags & UV_HANDLE_CLOSING));

  uv__io_start(stream->loop, &stream->io_watcher, POLLIN);

  /* connection_cb can close the server socket while we're
   * in the loop so check it on each iteration.
   */
  while (uv__stream_fd(stream) != -1) {
    assert(stream->accepted_fd == -1);

#if defined(UV_HAVE_KQUEUE)
    if (w->rcount <= 0)
      return;
#endif /* defined(UV_HAVE_KQUEUE) */

    err = uv__accept(uv__stream_fd(stream));
    if (err < 0) {
      if (err == UV_EAGAIN || err == UV__ERR(EWOULDBLOCK))
        return;  /* Not an error. */

      if (err == UV_ECONNABORTED)
        continue;  /* Ignore. Nothing we can do about that. */

      if (err == UV_EMFILE || err == UV_ENFILE) {
        err = uv__emfile_trick(loop, uv__stream_fd(stream));
        if (err == UV_EAGAIN || err == UV__ERR(EWOULDBLOCK))
          break;
      }

      /** accpet错误 **/
      stream->connection_cb(stream, err);
      continue;
    }

    UV_DEC_BACKLOG(w)
    /** 新连接就绪 **/
    stream->accepted_fd = err;
    stream->connection_cb(stream, 0);

    /** 未使用uv_accept连接或使用uv_close关闭连接 **/
    if (stream->accepted_fd != -1) {
      /** 先停止监听读事件, 待uv_accept重新打开 **/
      uv__io_stop(loop, &stream->io_watcher, POLLIN);
      return;
    }

    /** 休眠一会，分点给别的进程accept **/
    if (stream->type == UV_TCP &&
        (stream->flags & UV_HANDLE_TCP_SINGLE_ACCEPT)) {
      /* Give other processes a chance to accept connections. */
      struct timespec timeout = { 0, 1 };
      nanosleep(&timeout, NULL);
    }
  }
}


#undef UV_DEC_BACKLOG


int uv_accept(uv_stream_t* server, uv_stream_t* client) {
  int err;

  assert(server->loop == client->loop);

  if (server->accepted_fd == -1)
    return UV_EAGAIN;

  switch (client->type) {
    case UV_NAMED_PIPE:
    case UV_TCP:
      err = uv__stream_open(client,
                            server->accepted_fd,
                            UV_HANDLE_READABLE | UV_HANDLE_WRITABLE);
      if (err) {
        /* TODO handle error */
        uv__close(server->accepted_fd);
        goto done;
      }
      break;

    case UV_UDP:
      err = uv_udp_open((uv_udp_t*) client, server->accepted_fd);
      if (err) {
        uv__close(server->accepted_fd);
        goto done;
      }
      break;

    default:
      return UV_EINVAL;
  }

  /** 绑定内核分配的地址和端口 **/
  client->flags |= UV_HANDLE_BOUND;

done:
  /** 非single_accept时, 可连续调用uv_accept, 直到返回UV_EAGAIN **/
  if (server->queued_fds != NULL) {
    uv__stream_queued_fds_t* queued_fds;

    queued_fds = server->queued_fds;

    /* Read first */
    server->accepted_fd = queued_fds->fds[0];

    /* All read, free */
    assert(queued_fds->offset > 0);
    if (--queued_fds->offset == 0) {
      uv__free(queued_fds);
      server->queued_fds = NULL;
    } else {
      /* Shift rest */
      memmove(queued_fds->fds,
              queued_fds->fds + 1,
              queued_fds->offset * sizeof(*queued_fds->fds));
    }
  } else {
    /** 重新监听读事件 **/
    server->accepted_fd = -1;
    if (err == 0)
      uv__io_start(server->loop, &server->io_watcher, POLLIN);
  }
  return err;
}


int uv_listen(uv_stream_t* stream, int backlog, uv_connection_cb cb) {
  int err;

  switch (stream->type) {
  case UV_TCP:
    err = uv_tcp_listen((uv_tcp_t*)stream, backlog, cb);
    break;

  case UV_NAMED_PIPE:
    err = uv_pipe_listen((uv_pipe_t*)stream, backlog, cb);
    break;

  default:
    err = UV_EINVAL;
  }

  if (err == 0)
    uv__handle_start(stream);

  return err;
}


static void uv__drain(uv_stream_t* stream) {
  uv_shutdown_t* req;
  int err;

  assert(QUEUE_EMPTY(&stream->write_queue));
  /** 停止监听写事件, 防止一直触发写事件 **/
  uv__io_stop(stream->loop, &stream->io_watcher, POLLOUT);
  uv__stream_osx_interrupt_select(stream);

  /** 请求shutdown **/
  if ((stream->flags & UV_HANDLE_SHUTTING) &&
      !(stream->flags & UV_HANDLE_CLOSING) &&
      !(stream->flags & UV_HANDLE_SHUT)) {
    assert(stream->shutdown_req);

    req = stream->shutdown_req;
    stream->shutdown_req = NULL;
    stream->flags &= ~UV_HANDLE_SHUTTING;
    uv__req_unregister(stream->loop, req);

    err = 0;
    if (shutdown(uv__stream_fd(stream), SHUT_WR))
      err = UV__ERR(errno);

    if (err == 0)
      stream->flags |= UV_HANDLE_SHUT;

    /** 回调 **/
    if (req->cb != NULL)
      req->cb(req, err);
  }
}


static ssize_t uv__writev(int fd, struct iovec* vec, size_t n) {
  if (n == 1)
    return write(fd, vec->iov_base, vec->iov_len);
  else
    return writev(fd, vec, n);
}


static size_t uv__write_req_size(uv_write_t* req) {
  size_t size;

  assert(req->bufs != NULL);
  size = uv__count_bufs(req->bufs + req->write_index,
                        req->nbufs - req->write_index);
  assert(req->handle->write_queue_size >= size);

  return size;
}


/**
 * 更新缓冲区, 有数据写返回0, 否则返回1. 返回值仅和当前req有关
 **/
static int uv__write_req_update(uv_stream_t* stream,
                                uv_write_t* req,
                                size_t n) {
  uv_buf_t* buf;
  size_t len;

  assert(n <= stream->write_queue_size);
  /** 减少流待写数据字节数 **/
  stream->write_queue_size -= n;

  buf = req->bufs + req->write_index;

  do {
    len = n < buf->len ? n : buf->len;
    buf->base += len;
    buf->len -= len;
    buf += (buf->len == 0);  /* Advance to next buffer if this one is empty. */
    n -= len;
  } while (n > 0);

  req->write_index = buf - req->bufs;

  return req->write_index == req->nbufs;
}


static void uv__write_req_finish(uv_write_t* req) {
  uv_stream_t* stream = req->handle;

  /** 删除write_queue节点 **/
  QUEUE_REMOVE(&req->queue);

  /**
   * 只有在没有错误的情况下才释放. 如果出错, 我们在进行回调之前立即修改write_queue_size.
   * 我们不立即执行此操作的原因是, write_queue_size> 0是向用户发出信号的信号, 即他们应该停止写入,
   * 如果出现错误, 则应该这样做. libuv API的未来版本中有一些需要重新讨论的内容.
   **/
  if (req->error == 0) {
    if (req->bufs != req->bufsml)
      uv__free(req->bufs);
    req->bufs = NULL;
  }

  /** 将其添加到write_completed_queue中, uv_write执行完后将在uv__write_callbacks中回调 **/
  QUEUE_INSERT_TAIL(&stream->write_completed_queue, &req->queue);
  /** 此时流仍可写, 将io_wather加入pending_queue, 尝试处理下一个write_req  **/
  uv__io_feed(stream->loop, &stream->io_watcher);
}


static int uv__handle_fd(uv_handle_t* handle) {
  switch (handle->type) {
    case UV_NAMED_PIPE:
    case UV_TCP:
      return ((uv_stream_t*) handle)->io_watcher.fd;

    case UV_UDP:
      return ((uv_udp_t*) handle)->io_watcher.fd;

    default:
      return -1;
  }
}

static void uv__write(uv_stream_t* stream) {
  struct iovec* iov;
  QUEUE* q;
  uv_write_t* req;
  int iovmax;
  int iovcnt;
  ssize_t n;
  int err;

start:

  assert(uv__stream_fd(stream) >= 0);

  if (QUEUE_EMPTY(&stream->write_queue))
    return;

  q = QUEUE_HEAD(&stream->write_queue); /** write_queue队首 **/
  req = QUEUE_DATA(q, uv_write_t, queue);
  assert(req->handle == stream);

  /** 投放到iovec. 我们必须拥有自己的uv_buf_t而不是iovec, 因为Windows的WSABUF不是iovec. **/
  assert(sizeof(uv_buf_t) == sizeof(struct iovec));
  iov = (struct iovec*) &(req->bufs[req->write_index]);
  iovcnt = req->nbufs - req->write_index;

  /** 获取iovec最大大小 **/
  iovmax = uv__getiovmax();

  /** 防止writev返回EINVAL **/
  if (iovcnt > iovmax)
    iovcnt = iovmax;

  if (req->send_handle) {
    int fd_to_send;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    union {
      char data[64];
      struct cmsghdr alias;
    } scratch;

    if (uv__is_closing(req->send_handle)) {
      err = UV_EBADF;
      goto error;
    }

    fd_to_send = uv__handle_fd((uv_handle_t*) req->send_handle);

    memset(&scratch, 0, sizeof(scratch));

    assert(fd_to_send >= 0);

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = iovcnt;
    msg.msg_flags = 0;

    msg.msg_control = &scratch.alias;
    msg.msg_controllen = CMSG_SPACE(sizeof(fd_to_send));

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd_to_send));

    /* silence aliasing warning */
    {
      void* pv = CMSG_DATA(cmsg);
      int* pi = pv;
      *pi = fd_to_send;
    }

    do
      n = sendmsg(uv__stream_fd(stream), &msg, 0);
    while (n == -1 && RETRY_ON_WRITE_ERROR(errno));

    /* Ensure the handle isn't sent again in case this is a partial write. */
    if (n >= 0)
      req->send_handle = NULL;
  } else {
    do
      n = uv__writev(uv__stream_fd(stream), iov, iovcnt);
    while (n == -1 && RETRY_ON_WRITE_ERROR(errno)); /** (n == -1 && errno == EINTR) **/
  }

  /** 错误 **/
  if (n == -1 && !IS_TRANSIENT_WRITE_ERROR(errno, req->send_handle)) {
    err = UV__ERR(errno);
    goto error;
  }

  /** 更新缓冲区, 写完则从write_queue删除节点, 释放bufs **/
  if (n >= 0 && uv__write_req_update(stream, req, n)) {
    uv__write_req_finish(req);
    return;  /* TODO(bnoordhuis) Start trying to write the next request. */
  }

  /** 阻塞流则重试, 直到出错或写完 **/
  if (stream->flags & UV_HANDLE_BLOCKING_WRITES)
    goto start;

  /** 没写完, 继续监听写事件. 可以重复调用 **/
  uv__io_start(stream->loop, &stream->io_watcher, POLLOUT);

  /* Notify select() thread about state change */
  uv__stream_osx_interrupt_select(stream);

  return;

  /** 流上发生错误 **/
error:
  req->error = err;
  /** 将错误返回给用户 **/
  uv__write_req_finish(req);
  /** 停止监听写事件 **/
  uv__io_stop(stream->loop, &stream->io_watcher, POLLOUT);
  /** 没监听读事件则关闭流(稍后在uv__run_closing_handles中关闭) **/
  if (!uv__io_active(&stream->io_watcher, POLLIN))
    uv__handle_stop(stream);
  uv__stream_osx_interrupt_select(stream);
}


static void uv__write_callbacks(uv_stream_t* stream) {
  uv_write_t* req;
  QUEUE* q;
  QUEUE pq;

  if (QUEUE_EMPTY(&stream->write_completed_queue))
    return;

  QUEUE_MOVE(&stream->write_completed_queue, &pq);

  while (!QUEUE_EMPTY(&pq)) {
    /* Pop a req off write_completed_queue. */
    q = QUEUE_HEAD(&pq);
    req = QUEUE_DATA(q, uv_write_t, queue);
    QUEUE_REMOVE(q);
    uv__req_unregister(stream->loop, req);

    if (req->bufs != NULL) {
      stream->write_queue_size -= uv__write_req_size(req);
      if (req->bufs != req->bufsml)
        uv__free(req->bufs);
      req->bufs = NULL;
    }

    /* NOTE: call callback AFTER freeing the request data. */
    /** 调用用户回调 **/
    if (req->cb)
      req->cb(req, req->error);
  }
}


uv_handle_type uv__handle_type(int fd) {
  struct sockaddr_storage ss;
  socklen_t sslen;
  socklen_t len;
  int type;

  memset(&ss, 0, sizeof(ss));
  sslen = sizeof(ss);

  if (getsockname(fd, (struct sockaddr*)&ss, &sslen))
    return UV_UNKNOWN_HANDLE;

  len = sizeof type;

  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len))
    return UV_UNKNOWN_HANDLE;

  if (type == SOCK_STREAM) {
#if defined(_AIX) || defined(__DragonFly__)
    /* on AIX/DragonFly the getsockname call returns an empty sa structure
     * for sockets of type AF_UNIX.  For all other types it will
     * return a properly filled in structure.
     */
    if (sslen == 0)
      return UV_NAMED_PIPE;
#endif
    switch (ss.ss_family) {
      case AF_UNIX:
        return UV_NAMED_PIPE;
      case AF_INET:
      case AF_INET6:
        return UV_TCP;
      }
  }

  if (type == SOCK_DGRAM &&
      (ss.ss_family == AF_INET || ss.ss_family == AF_INET6))
    return UV_UDP;

  return UV_UNKNOWN_HANDLE;
}


static void uv__stream_eof(uv_stream_t* stream, const uv_buf_t* buf) {
  stream->flags |= UV_HANDLE_READ_EOF;
  stream->flags &= ~UV_HANDLE_READING;
  uv__io_stop(stream->loop, &stream->io_watcher, POLLIN);
  if (!uv__io_active(&stream->io_watcher, POLLOUT))
    uv__handle_stop(stream);
  uv__stream_osx_interrupt_select(stream);
  stream->read_cb(stream, UV_EOF, buf);
}


static int uv__stream_queue_fd(uv_stream_t* stream, int fd) {
  uv__stream_queued_fds_t* queued_fds;
  unsigned int queue_size;

  queued_fds = stream->queued_fds;
  if (queued_fds == NULL) {
    queue_size = 8;
    queued_fds = uv__malloc((queue_size - 1) * sizeof(*queued_fds->fds) +
                            sizeof(*queued_fds));
    if (queued_fds == NULL)
      return UV_ENOMEM;
    queued_fds->size = queue_size;
    queued_fds->offset = 0;
    stream->queued_fds = queued_fds;

    /* Grow */
  } else if (queued_fds->size == queued_fds->offset) {
    queue_size = queued_fds->size + 8;
    queued_fds = uv__realloc(queued_fds,
                             (queue_size - 1) * sizeof(*queued_fds->fds) +
                              sizeof(*queued_fds));

    /*
     * Allocation failure, report back.
     * NOTE: if it is fatal - sockets will be closed in uv__stream_close
     */
    if (queued_fds == NULL)
      return UV_ENOMEM;
    queued_fds->size = queue_size;
    stream->queued_fds = queued_fds;
  }

  /* Put fd in a queue */
  queued_fds->fds[queued_fds->offset++] = fd;

  return 0;
}


#if defined(__PASE__)
/* on IBMi PASE the control message length can not exceed 256. */
# define UV__CMSG_FD_COUNT 60
#else
# define UV__CMSG_FD_COUNT 64
#endif
#define UV__CMSG_FD_SIZE (UV__CMSG_FD_COUNT * sizeof(int))


static int uv__stream_recv_cmsg(uv_stream_t* stream, struct msghdr* msg) {
  struct cmsghdr* cmsg;

  for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    char* start;
    char* end;
    int err;
    void* pv;
    int* pi;
    unsigned int i;
    unsigned int count;

    if (cmsg->cmsg_type != SCM_RIGHTS) {
      fprintf(stderr, "ignoring non-SCM_RIGHTS ancillary data: %d\n",
          cmsg->cmsg_type);
      continue;
    }

    /* silence aliasing warning */
    pv = CMSG_DATA(cmsg);
    pi = pv;

    /* Count available fds */
    start = (char*) cmsg;
    end = (char*) cmsg + cmsg->cmsg_len;
    count = 0;
    while (start + CMSG_LEN(count * sizeof(*pi)) < end)
      count++;
    assert(start + CMSG_LEN(count * sizeof(*pi)) == end);

    for (i = 0; i < count; i++) {
      /* Already has accepted fd, queue now */
      if (stream->accepted_fd != -1) {
        err = uv__stream_queue_fd(stream, pi[i]);
        if (err != 0) {
          /* Close rest */
          for (; i < count; i++)
            uv__close(pi[i]);
          return err;
        }
      } else {
        stream->accepted_fd = pi[i];
      }
    }
  }

  return 0;
}


#ifdef __clang__
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wgnu-folding-constant"
# pragma clang diagnostic ignored "-Wvla-extension"
#endif

static void uv__read(uv_stream_t* stream) {
  uv_buf_t buf;
  ssize_t nread;
  struct msghdr msg;
  char cmsg_space[CMSG_SPACE(UV__CMSG_FD_SIZE)];
  int count;
  int err;
  int is_ipc;

  stream->flags &= ~UV_HANDLE_READ_PARTIAL;

  /* Prevent loop starvation when the data comes in as fast as (or faster than)
   * we can read it. XXX Need to rearm fd if we switch to edge-triggered I/O.
   */
  /** 防止阻塞 **/
  count = 32;

  is_ipc = stream->type == UV_NAMED_PIPE && ((uv_pipe_t*) stream)->ipc;

  /* XXX: Maybe instead of having UV_HANDLE_READING we just test if
   * tcp->read_cb is NULL or not?
   */
  while (stream->read_cb
      && (stream->flags & UV_HANDLE_READING)
      && (count-- > 0)) {
    assert(stream->alloc_cb != NULL);

    /** 用户分配内存, uv不会保存, 在回调用释放, 也可以重复使用 **/
    buf = uv_buf_init(NULL, 0);
    stream->alloc_cb((uv_handle_t*)stream, 64 * 1024, &buf);
    if (buf.base == NULL || buf.len == 0) {
      /* User indicates it can't or won't handle the read. */
      stream->read_cb(stream, UV_ENOBUFS, &buf);
      return;
    }

    assert(buf.base != NULL);
    assert(uv__stream_fd(stream) >= 0);

    if (!is_ipc) {
      do {
        nread = read(uv__stream_fd(stream), buf.base, buf.len);
      }
      while (nread < 0 && errno == EINTR);
    } else {
      /* ipc uses recvmsg */
      msg.msg_flags = 0;
      msg.msg_iov = (struct iovec*) &buf;
      msg.msg_iovlen = 1;
      msg.msg_name = NULL;
      msg.msg_namelen = 0;
      /* Set up to receive a descriptor even if one isn't in the message */
      msg.msg_controllen = sizeof(cmsg_space);
      msg.msg_control = cmsg_space;

      do {
        nread = uv__recvmsg(uv__stream_fd(stream), &msg, 0);
      }
      while (nread < 0 && errno == EINTR);
    }

    if (nread < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Wait for the next one. */
        if (stream->flags & UV_HANDLE_READING) {
          uv__io_start(stream->loop, &stream->io_watcher, POLLIN);
          uv__stream_osx_interrupt_select(stream);
        }
        stream->read_cb(stream, 0, &buf);
#if defined(__CYGWIN__) || defined(__MSYS__)
      } else if (errno == ECONNRESET && stream->type == UV_NAMED_PIPE) {
        /** EOF **/
        uv__stream_eof(stream, &buf);
        return;
#endif
      } else {
        /* Error. User should call uv_close(). */
        stream->read_cb(stream, UV__ERR(errno), &buf);
        if (stream->flags & UV_HANDLE_READING) {
          stream->flags &= ~UV_HANDLE_READING;
          uv__io_stop(stream->loop, &stream->io_watcher, POLLIN);
          if (!uv__io_active(&stream->io_watcher, POLLOUT))
            uv__handle_stop(stream);
          uv__stream_osx_interrupt_select(stream);
        }
      }
      return;
    } else if (nread == 0) {
      /** EOF **/
      uv__stream_eof(stream, &buf);
      return;
    } else {
      /* Successful read */
      ssize_t buflen = buf.len;

      if (is_ipc) {
        err = uv__stream_recv_cmsg(stream, &msg);
        if (err != 0) {
          stream->read_cb(stream, err, &buf);
          return;
        }
      }

#if defined(__MVS__)
      if (is_ipc && msg.msg_controllen > 0) {
        uv_buf_t blankbuf;
        int nread;
        struct iovec *old;

        blankbuf.base = 0;
        blankbuf.len = 0;
        old = msg.msg_iov;
        msg.msg_iov = (struct iovec*) &blankbuf;
        nread = 0;
        do {
          nread = uv__recvmsg(uv__stream_fd(stream), &msg, 0);
          err = uv__stream_recv_cmsg(stream, &msg);
          if (err != 0) {
            stream->read_cb(stream, err, &buf);
            msg.msg_iov = old;
            return;
          }
        } while (nread == 0 && msg.msg_controllen > 0);
        msg.msg_iov = old;
      }
#endif
      /** 回调 **/
      stream->read_cb(stream, nread, &buf);

      /* Return if we didn't fill the buffer, there is no more data to read. */
      /** tcp缓冲区为空了 **/
      if (nread < buflen) {
        stream->flags |= UV_HANDLE_READ_PARTIAL;
        return;
      }

      /** tcp缓冲区仍有数据, continue **/
    }
  }
}


#ifdef __clang__
# pragma clang diagnostic pop
#endif

#undef UV__CMSG_FD_COUNT
#undef UV__CMSG_FD_SIZE


int uv_shutdown(uv_shutdown_t* req, uv_stream_t* stream, uv_shutdown_cb cb) {
  assert(stream->type == UV_TCP ||
         stream->type == UV_TTY ||
         stream->type == UV_NAMED_PIPE);

  if (!(stream->flags & UV_HANDLE_WRITABLE) ||
      stream->flags & UV_HANDLE_SHUT ||
      stream->flags & UV_HANDLE_SHUTTING ||
      uv__is_closing(stream)) {
    return UV_ENOTCONN;
  }

  assert(uv__stream_fd(stream) >= 0);

  /* Initialize request */
  uv__req_init(stream->loop, req, UV_SHUTDOWN);
  req->handle = stream;
  req->cb = cb;
  stream->shutdown_req = req;
  stream->flags |= UV_HANDLE_SHUTTING;

  /** 先把数据写完, 然后uv__stream_io中的uv__drain中调用shutdown **/
  uv__io_start(stream->loop, &stream->io_watcher, POLLOUT);
  uv__stream_osx_interrupt_select(stream);

  return 0;
}

/** 分派i/o事件 **/
static void uv__stream_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  uv_stream_t* stream;

  stream = container_of(w, uv_stream_t, io_watcher);

  assert(stream->type == UV_TCP ||
         stream->type == UV_NAMED_PIPE ||
         stream->type == UV_TTY);
  assert(!(stream->flags & UV_HANDLE_CLOSING));

  if (stream->connect_req) {
    uv__stream_connect(stream);
    return;
  }

  assert(uv__stream_fd(stream) >= 0);

  /* Ignore POLLHUP here. Even if it's set, there may still be data to read. */
  if (events & (POLLIN | POLLERR | POLLHUP))
    uv__read(stream);

  if (uv__stream_fd(stream) == -1)
    return;  /* read_cb closed stream. */

  /* Short-circuit iff POLLHUP is set, the user is still interested in read
   * events and uv__read() reported a partial read but not EOF. If the EOF
   * flag is set, uv__read() called read_cb with err=UV_EOF and we don't
   * have to do anything. If the partial read flag is not set, we can't
   * report the EOF yet because there is still data to read.
   */
  if ((events & POLLHUP) &&
      (stream->flags & UV_HANDLE_READING) &&
      (stream->flags & UV_HANDLE_READ_PARTIAL) &&
      !(stream->flags & UV_HANDLE_READ_EOF)) {
    uv_buf_t buf = { NULL, 0 };
    uv__stream_eof(stream, &buf);
  }

  if (uv__stream_fd(stream) == -1)
    return;  /* read_cb closed stream. */

  if (events & (POLLOUT | POLLERR | POLLHUP)) {
    uv__write(stream);
    uv__write_callbacks(stream);

    /** write_queue耗尽 **/
    if (QUEUE_EMPTY(&stream->write_queue))
      uv__drain(stream);
  }
}


static void uv__stream_connect(uv_stream_t* stream) {
  int error;
  uv_connect_t* req = stream->connect_req;
  socklen_t errorsize = sizeof(int);

  assert(stream->type == UV_TCP || stream->type == UV_NAMED_PIPE);
  assert(req);

  if (stream->delayed_error) {
    /** 为了消除第一次连接上同步报告的unix错误之间的差异, 可以将其延迟到next tick. */
    error = stream->delayed_error;
    stream->delayed_error = 0;
  } else {
    /**
     * 我们在直接调用connect(2)之后被调用. 为了确定我们是否出错或成功, 必须调用getsockopt从内核获取错误.
     **/
    assert(uv__stream_fd(stream) >= 0);
    getsockopt(uv__stream_fd(stream),
               SOL_SOCKET,
               SO_ERROR,
               &error,
               &errorsize);
    error = UV__ERR(error);
  }

  if (error == UV__ERR(EINPROGRESS))
    return;

  stream->connect_req = NULL;
  uv__req_unregister(stream->loop, req);

  /** 停止监听 **/
  if (error < 0 || QUEUE_EMPTY(&stream->write_queue)) {
    uv__io_stop(stream->loop, &stream->io_watcher, POLLOUT);
  }

  /** 回调 **/
  if (req->cb)
    req->cb(req, error);

  if (uv__stream_fd(stream) == -1)
    return;

  /** 清除write_req **/
  if (error < 0) {
    uv__stream_flush_write_queue(stream, UV_ECANCELED);
    uv__write_callbacks(stream);
  }
}


int uv_write2(uv_write_t* req,
              uv_stream_t* stream,
              const uv_buf_t bufs[],
              unsigned int nbufs,
              uv_stream_t* send_handle,
              uv_write_cb cb) {
  int empty_queue;

  assert(nbufs > 0);
  assert((stream->type == UV_TCP ||
          stream->type == UV_NAMED_PIPE ||
          stream->type == UV_TTY) &&
         "uv_write (unix) does not yet support other types of streams");

  if (uv__stream_fd(stream) < 0)
    return UV_EBADF;

  /** 流不可写 **/
  if (!(stream->flags & UV_HANDLE_WRITABLE))
    return UV_EPIPE;

  if (send_handle) {
    if (stream->type != UV_NAMED_PIPE || !((uv_pipe_t*)stream)->ipc)
      return UV_EINVAL;

    /**
     * 我们滥用uv_write2()通过UDP处理将其发送给子进程. 不要在这些句柄上调用uv__stream_fd(),
     * 它是OS X上评估为在uv_stream_t上运行且具有几个OS X特定字段的函数的宏. 在其他Unices上,
     * (handle)->io_watcher.fd可以起作用, 但这只是偶然的.
     **/
    if (uv__handle_fd((uv_handle_t*) send_handle) < 0)
      return UV_EBADF;

#if defined(__CYGWIN__) || defined(__MSYS__)
    /* Cygwin recvmsg always sets msg_controllen to zero, so we cannot send it.
       See https://github.com/mirror/newlib-cygwin/blob/86fc4bf0/winsup/cygwin/fhandler_socket.cc#L1736-L1743 */
    return UV_ENOSYS;
#endif
  }

  /**
   * 即使write_queue为空, write_queue_size > 0也合法. 意味着write_completed_queue中存在错误状态请求,
   * 这些错误状态请求将在以后修改write_queue_size, 另请参见uv__write_req_finish().
   * 我们可以检查一下write_queue是否为空, 但这意味着当我们知道该句柄处于错误模式时, 将执行一次write()系统调用.
   **/
  empty_queue = (stream->write_queue_size == 0);

  /** 设置请求类型并注册请求 **/
  uv__req_init(stream->loop, req, UV_WRITE);
  req->cb = cb;
  req->handle = stream;
  req->error = 0;
  req->send_handle = send_handle;
  QUEUE_INIT(&req->queue);

  /** 默认iovec大小为4, 超过4时扩容 **/
  req->bufs = req->bufsml;
  if (nbufs > ARRAY_SIZE(req->bufsml))
    req->bufs = uv__malloc(nbufs * sizeof(bufs[0]));
  if (req->bufs == NULL)
    return UV_ENOMEM;

  /** 拷贝iovec, 不拷贝数据 **/
  memcpy(req->bufs, bufs, nbufs * sizeof(bufs[0]));
  req->nbufs = nbufs;
  req->write_index = 0;
  /** 增加流待写字节数 **/
  stream->write_queue_size += uv__count_bufs(bufs, nbufs);

  /** 将request添加到write_queue. **/
  QUEUE_INSERT_TAIL(&stream->write_queue, &req->queue);

  /** 如果此函数开始时队列为空, 则应尝试立即进行写操作. 则, 启动write_watcher并等待fd变为可写状态. **/
  if (stream->connect_req) {
    /* Still connecting, do nothing. */
    /** 未连接, 不能写. 次握手成功后connect_req会置NULL **/
  }
  else if (empty_queue) {
    /** 待写队列为空, 直接写, 允许阻塞写 **/
    uv__write(stream);
  }
  else {
    /** 待写队列中有数据时不能使用阻塞流, 否则会乱序 **/
    assert(!(stream->flags & UV_HANDLE_BLOCKING_WRITES));
    /** 注册写事件 **/
    uv__io_start(stream->loop, &stream->io_watcher, POLLOUT);
    uv__stream_osx_interrupt_select(stream);
  }

  return 0;
}


/** 在调用回调之前, 要写入的缓冲区必须保持有效. uv_buf_t数组不是必需的 **/
int uv_write(uv_write_t* req,
             uv_stream_t* handle,
             const uv_buf_t bufs[],
             unsigned int nbufs,
             uv_write_cb cb) {
  return uv_write2(req, handle, bufs, nbufs, NULL, cb);
}


void uv_try_write_cb(uv_write_t* req, int status) {
  /* Should not be called */
  abort();
}

/** 如果不能马上写, 不会投递write_req **/
int uv_try_write(uv_stream_t* stream,
                 const uv_buf_t bufs[],
                 unsigned int nbufs) {
  int r;
  int has_pollout;
  size_t written;
  size_t req_size;
  uv_write_t req;

  /* Connecting or already writing some data */
  if (stream->connect_req != NULL || stream->write_queue_size != 0)
    return UV_EAGAIN;

  has_pollout = uv__io_active(&stream->io_watcher, POLLOUT);

  r = uv_write(&req, stream, bufs, nbufs, uv_try_write_cb);
  if (r != 0)
    return r;

  /* Remove not written bytes from write queue size */
  written = uv__count_bufs(bufs, nbufs);
  if (req.bufs != NULL)
    req_size = uv__write_req_size(&req);
  else
    req_size = 0;
  written -= req_size;
  stream->write_queue_size -= req_size;

  /* Unqueue request, regardless of immediateness */
  QUEUE_REMOVE(&req.queue);
  uv__req_unregister(stream->loop, &req);
  if (req.bufs != req.bufsml)
    uv__free(req.bufs);
  req.bufs = NULL;

  /* Do not poll for writable, if we wasn't before calling this */
  if (!has_pollout) {
    uv__io_stop(stream->loop, &stream->io_watcher, POLLOUT);
    uv__stream_osx_interrupt_select(stream);
  }

  if (written == 0 && req_size != 0)
    return req.error < 0 ? req.error : UV_EAGAIN;
  else
    return written;
}

/**
 * 切换流状态从paused-->flowing, 当有数据到达流时,
 * 会调用alloc_cb分配冲区, 将数据写入其中并调用read_cb,
 * 将数据返回给用户 **/
int uv_read_start(uv_stream_t* stream,
                  uv_alloc_cb alloc_cb,
                  uv_read_cb read_cb) {
  assert(stream->type == UV_TCP || stream->type == UV_NAMED_PIPE ||
      stream->type == UV_TTY);

  /** 流已关闭 **/
  if (stream->flags & UV_HANDLE_CLOSING)
    return UV_EINVAL;

  /** 流不可读 **/
  if (!(stream->flags & UV_HANDLE_READABLE))
    return UV_ENOTCONN;

  /** UV_HANDLE_READING标志与tcp的状态无关, 它只是表示用户期望的状态 **/
  stream->flags |= UV_HANDLE_READING;

  /* TODO: try to do the read inline? */
  /* TODO: keep track of tcp state. If we've gotten a EOF then we should
   * not start the IO watcher.
   */
  assert(uv__stream_fd(stream) >= 0);
  assert(alloc_cb);

  stream->read_cb = read_cb;
  stream->alloc_cb = alloc_cb;

  /** 注册读事件 **/
  uv__io_start(stream->loop, &stream->io_watcher, POLLIN);
  /** 激活handle, 防止loop退出 **/
  uv__handle_start(stream);
  uv__stream_osx_interrupt_select(stream);

  return 0;
}

/** 切换流状态从flowing-->paused **/
int uv_read_stop(uv_stream_t* stream) {
  if (!(stream->flags & UV_HANDLE_READING))
    return 0;

  /** 取消状态 **/
  stream->flags &= ~UV_HANDLE_READING;
  /** 停止监听读事件 **/
  uv__io_stop(stream->loop, &stream->io_watcher, POLLIN);
  /** 取消激活handle **/
  if (!uv__io_active(&stream->io_watcher, POLLOUT))
    uv__handle_stop(stream);
  uv__stream_osx_interrupt_select(stream);

  stream->read_cb = NULL;
  stream->alloc_cb = NULL;
  return 0;
}

/** 流是否可读 **/
int uv_is_readable(const uv_stream_t* stream) {
  return !!(stream->flags & UV_HANDLE_READABLE);
}

/** 流是否可写 **/
int uv_is_writable(const uv_stream_t* stream) {
  return !!(stream->flags & UV_HANDLE_WRITABLE);
}


#if defined(__APPLE__)
int uv___stream_fd(const uv_stream_t* handle) {
  const uv__stream_select_t* s;

  assert(handle->type == UV_TCP ||
         handle->type == UV_TTY ||
         handle->type == UV_NAMED_PIPE);

  s = handle->select;
  if (s != NULL)
    return s->fd;

  return handle->io_watcher.fd;
}
#endif /* defined(__APPLE__) */

/** 关闭流: 停止事件监听, 关闭fd, 关闭accpet_fd, reqests在uv__stream_destroy中清除 **/
void uv__stream_close(uv_stream_t* handle) {
  unsigned int i;
  uv__stream_queued_fds_t* queued_fds;

#if defined(__APPLE__)
  /* Terminate select loop first */
  if (handle->select != NULL) {
    uv__stream_select_t* s;

    s = handle->select;

    uv_sem_post(&s->close_sem);
    uv_sem_post(&s->async_sem);
    uv__stream_osx_interrupt_select(handle);
    uv_thread_join(&s->thread);
    uv_sem_destroy(&s->close_sem);
    uv_sem_destroy(&s->async_sem);
    uv__close(s->fake_fd);
    uv__close(s->int_fd);
    uv_close((uv_handle_t*) &s->async, uv__stream_osx_cb_close);

    handle->select = NULL;
  }
#endif /* defined(__APPLE__) */

  /** 停止所有事件的监听 **/
  uv__io_close(handle->loop, &handle->io_watcher);
  /** 主要是重置cb, 唤醒loop(OS X) **/
  uv_read_stop(handle);
  uv__handle_stop(handle);
  handle->flags &= ~(UV_HANDLE_READABLE | UV_HANDLE_WRITABLE);

  /** 关闭流本身的fd **/
  if (handle->io_watcher.fd != -1) {
    /* Don't close stdio file descriptors.  Nothing good comes from it. */
    if (handle->io_watcher.fd > STDERR_FILENO)
      uv__close(handle->io_watcher.fd);
    handle->io_watcher.fd = -1;
  }

  /** 如果是listener, 关闭所有accept的fd **/
  if (handle->accepted_fd != -1) {
    uv__close(handle->accepted_fd);
    handle->accepted_fd = -1;
  }
  if (handle->queued_fds != NULL) {
    queued_fds = handle->queued_fds;
    for (i = 0; i < queued_fds->offset; i++)
      uv__close(queued_fds->fds[i]);
    uv__free(handle->queued_fds);
    handle->queued_fds = NULL;
  }

  assert(!uv__io_active(&handle->io_watcher, POLLIN | POLLOUT));
}


int uv_stream_set_blocking(uv_stream_t* handle, int blocking) {
  /* Don't need to check the file descriptor, uv__nonblock()
   * will fail with EBADF if it's not valid.
   */
  return uv__nonblock(uv__stream_fd(handle), !blocking);
}
