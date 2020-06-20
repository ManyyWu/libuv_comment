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

#include <assert.h>
#include <stdlib.h>

#include "uv.h"
#include "internal.h"
#include "handle-inl.h"
#include "stream-inl.h"
#include "req-inl.h"


/*
 * Threshold of active tcp streams for which to preallocate tcp read buffers.
 * (Due to node slab allocator performing poorly under this pattern,
 *  the optimization is temporarily disabled (threshold=0).  This will be
 *  revisited once node allocator is improved.)
 */
const unsigned int uv_active_tcp_streams_threshold = 0;

/*
 * Number of simultaneous pending AcceptEx calls.
 */
const unsigned int uv_simultaneous_server_accepts = 32;

/* A zero-size buffer for use by uv_tcp_read */
static char uv_zero_[] = "";

static int uv__tcp_nodelay(uv_tcp_t* handle, SOCKET socket, int enable) {
  if (setsockopt(socket,
                 IPPROTO_TCP,
                 TCP_NODELAY,
                 (const char*)&enable,
                 sizeof enable) == -1) {
    return WSAGetLastError();
  }
  return 0;
}


static int uv__tcp_keepalive(uv_tcp_t* handle, SOCKET socket, int enable, unsigned int delay) {
  if (setsockopt(socket,
                 SOL_SOCKET,
                 SO_KEEPALIVE,
                 (const char*)&enable,
                 sizeof enable) == -1) {
    return WSAGetLastError();
  }

  if (enable && setsockopt(socket,
                           IPPROTO_TCP,
                           TCP_KEEPALIVE,
                           (const char*)&delay,
                           sizeof delay) == -1) {
    return WSAGetLastError();
  }

  return 0;
}


static int uv_tcp_set_socket(uv_loop_t* loop,
                             uv_tcp_t* handle,
                             SOCKET socket,
                             int family,
                             int imported) {
  DWORD yes = 1;
  int non_ifs_lsp;
  int err;

  if (handle->socket != INVALID_SOCKET)
    return UV_EBUSY;

  /** nonblocking **/
  if (ioctlsocket(socket, FIONBIO, &yes) == SOCKET_ERROR) {
    return WSAGetLastError();
  }

  /** cloexec **/
  if (!SetHandleInformation((HANDLE) socket, HANDLE_FLAG_INHERIT, 0))
    return GetLastError();

  /** 与IOCP关联 **/
  if (CreateIoCompletionPort((HANDLE)socket,
                             loop->iocp,
                             (ULONG_PTR)socket,
                             0) == NULL) {
    if (imported) {
      handle->flags |= UV_HANDLE_EMULATE_IOCP;
    } else {
      return GetLastError();
    }
  }

  if (family == AF_INET6) {
    non_ifs_lsp = uv_tcp_non_ifs_lsp_ipv6;
  } else {
    non_ifs_lsp = uv_tcp_non_ifs_lsp_ipv4;
  }

  /**
  * https://support.microsoft.com/en-hk/help/2568167/setfilecompletionnotificationmodes-api-causes-an-i-o-completion-port-n
  * https://tboox.org/cn/2018/08/16/coroutine-iocp-some-issues/
  * 如果安装了non-IFS LSP, SetFileCompletionNotificationModes API会导致I/O完成端口无法正常工作.
  * FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: 如果WSAXxxx如果立即成功返回, 那么不会再队列化I/O Event了.
  **/
  if (!(handle->flags & UV_HANDLE_EMULATE_IOCP) && !non_ifs_lsp) {
    UCHAR sfcnm_flags =
        FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    if (!SetFileCompletionNotificationModes((HANDLE) socket, sfcnm_flags))
      return GetLastError();
    handle->flags |= UV_HANDLE_SYNC_BYPASS_IOCP;
  }

  /** 禁用Nagle算法 **/
  if (handle->flags & UV_HANDLE_TCP_NODELAY) {
    err = uv__tcp_nodelay(handle, socket, 1);
    if (err)
      return err;
  }

  /** keepalive **/
  if (handle->flags & UV_HANDLE_TCP_KEEPALIVE) {
    err = uv__tcp_keepalive(handle, socket, 1, 60);
    if (err)
      return err;
  }

  handle->socket = socket;

  if (family == AF_INET6) {
    handle->flags |= UV_HANDLE_IPV6;
  } else {
    assert(!(handle->flags & UV_HANDLE_IPV6));
  }

  return 0;
}


int uv_tcp_init_ex(uv_loop_t* loop, uv_tcp_t* handle, unsigned int flags) {
  int domain;

  /* Use the lower 8 bits for the domain */
  domain = flags & 0xFF;
  if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNSPEC)
    return UV_EINVAL;

  if (flags & ~0xFF)
    return UV_EINVAL;

  uv_stream_init(loop, (uv_stream_t*) handle, UV_TCP);
  handle->tcp.serv.accept_reqs = NULL;
  handle->tcp.serv.pending_accepts = NULL;
  handle->socket = INVALID_SOCKET;
  handle->reqs_pending = 0;
  handle->tcp.serv.func_acceptex = NULL;
  handle->tcp.conn.func_connectex = NULL;
  handle->tcp.serv.processed_accepts = 0;
  handle->delayed_error = 0;

  /* If anything fails beyond this point we need to remove the handle from
   * the handle queue, since it was added by uv__handle_init in uv_stream_init.
   */

  if (domain != AF_UNSPEC) {
    SOCKET sock;
    DWORD err;

    /** 创建套接字 **/
    sock = socket(domain, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
      err = WSAGetLastError();
      QUEUE_REMOVE(&handle->handle_queue);
      return uv_translate_sys_error(err);
    }

    /** 将socket关联到handle, nonblocking, cloexec, 关联iocp， 并设置套接字选项 **/
    err = uv_tcp_set_socket(handle->loop, handle, sock, domain, 0);
    if (err) {
      closesocket(sock);
      QUEUE_REMOVE(&handle->handle_queue);
      return uv_translate_sys_error(err);
    }

  }

  return 0;
}


int uv_tcp_init(uv_loop_t* loop, uv_tcp_t* handle) {
  return uv_tcp_init_ex(loop, handle, AF_UNSPEC);
}


void uv_tcp_endgame(uv_loop_t* loop, uv_tcp_t* handle) {
  int err;
  unsigned int i;
  uv_tcp_accept_t* req;

  if (handle->flags & UV_HANDLE_CONNECTION &&
      handle->stream.conn.shutdown_req != NULL &&
      handle->stream.conn.write_reqs_pending == 0) {

    UNREGISTER_HANDLE_REQ(loop, handle, handle->stream.conn.shutdown_req);

    err = 0;
    if (handle->flags & UV_HANDLE_CLOSING) {
      err = ERROR_OPERATION_ABORTED;
    } else if (shutdown(handle->socket, SD_SEND) == SOCKET_ERROR) {
      err = WSAGetLastError();
    }

    if (handle->stream.conn.shutdown_req->cb) {
      handle->stream.conn.shutdown_req->cb(handle->stream.conn.shutdown_req,
                               uv_translate_sys_error(err));
    }

    handle->stream.conn.shutdown_req = NULL;
    DECREASE_PENDING_REQ_COUNT(handle);
    return;
  }

  if (handle->flags & UV_HANDLE_CLOSING &&
      handle->reqs_pending == 0) {
    assert(!(handle->flags & UV_HANDLE_CLOSED));

    if (!(handle->flags & UV_HANDLE_TCP_SOCKET_CLOSED)) {
      closesocket(handle->socket);
      handle->socket = INVALID_SOCKET;
      handle->flags |= UV_HANDLE_TCP_SOCKET_CLOSED;
    }

    if (!(handle->flags & UV_HANDLE_CONNECTION) && handle->tcp.serv.accept_reqs) {
      if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
        for (i = 0; i < uv_simultaneous_server_accepts; i++) {
          req = &handle->tcp.serv.accept_reqs[i];
          if (req->wait_handle != INVALID_HANDLE_VALUE) {
            UnregisterWait(req->wait_handle);
            req->wait_handle = INVALID_HANDLE_VALUE;
          }
          if (req->event_handle != NULL) {
            CloseHandle(req->event_handle);
            req->event_handle = NULL;
          }
        }
      }

      uv__free(handle->tcp.serv.accept_reqs);
      handle->tcp.serv.accept_reqs = NULL;
    }

    if (handle->flags & UV_HANDLE_CONNECTION &&
        handle->flags & UV_HANDLE_EMULATE_IOCP) {
      if (handle->read_req.wait_handle != INVALID_HANDLE_VALUE) {
        UnregisterWait(handle->read_req.wait_handle);
        handle->read_req.wait_handle = INVALID_HANDLE_VALUE;
      }
      if (handle->read_req.event_handle != NULL) {
        CloseHandle(handle->read_req.event_handle);
        handle->read_req.event_handle = NULL;
      }
    }

    uv__handle_close(handle);
    loop->active_tcp_streams--;
  }
}


/* Unlike on Unix, here we don't set SO_REUSEADDR, because it doesn't just
 * allow binding to addresses that are in use by sockets in TIME_WAIT, it
 * effectively allows 'stealing' a port which is in use by another application.
 *
 * SO_EXCLUSIVEADDRUSE is also not good here because it does check all sockets,
 * regardless of state, so we'd get an error even if the port is in use by a
 * socket in TIME_WAIT state.
 *
 * See issue #1360.
 *
 */
static int uv_tcp_try_bind(uv_tcp_t* handle,
                           const struct sockaddr* addr,
                           unsigned int addrlen,
                           unsigned int flags) {
  DWORD err;
  int r;

  if (handle->socket == INVALID_SOCKET) {
    SOCKET sock;

    /* Cannot set IPv6-only mode on non-IPv6 socket. */
    if ((flags & UV_TCP_IPV6ONLY) && addr->sa_family != AF_INET6)
      return ERROR_INVALID_PARAMETER;

    sock = socket(addr->sa_family, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
      return WSAGetLastError();
    }

    /** 将socket关联到handle, nonblocking, cloexec, 关联iocp， 并设置套接字选项 **/
    err = uv_tcp_set_socket(handle->loop, handle, sock, addr->sa_family, 0);
    if (err) {
      closesocket(sock);
      return err;
    }
  }

#ifdef IPV6_V6ONLY
  if (addr->sa_family == AF_INET6) {
    int on;

    on = (flags & UV_TCP_IPV6ONLY) != 0;

    /* TODO: how to handle errors? This may fail if there is no ipv4 stack
     * available, or when run on XP/2003 which have no support for dualstack
     * sockets. For now we're silently ignoring the error. */
    setsockopt(handle->socket,
               IPPROTO_IPV6,
               IPV6_V6ONLY,
               (const char*)&on,
               sizeof on);
  }
#endif

  r = bind(handle->socket, addr, addrlen);

  if (r == SOCKET_ERROR) {
    err = WSAGetLastError();
    if (err == WSAEADDRINUSE) {
      /* Some errors are not to be reported until connect() or listen() */
      handle->delayed_error = err;
    } else {
      return err;
    }
  }

  handle->flags |= UV_HANDLE_BOUND;

  return 0;
}


static void CALLBACK post_completion(void* context, BOOLEAN timed_out) {
  uv_req_t* req;
  uv_tcp_t* handle;

  req = (uv_req_t*) context;
  assert(req != NULL);
  handle = (uv_tcp_t*)req->data;
  assert(handle != NULL);
  assert(!timed_out);

  if (!PostQueuedCompletionStatus(handle->loop->iocp,
                                  req->u.io.overlapped.InternalHigh,
                                  0,
                                  &req->u.io.overlapped)) {
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");
  }
}


static void CALLBACK post_write_completion(void* context, BOOLEAN timed_out) {
  uv_write_t* req;
  uv_tcp_t* handle;

  req = (uv_write_t*) context;
  assert(req != NULL);
  handle = (uv_tcp_t*)req->handle;
  assert(handle != NULL);
  assert(!timed_out);

  if (!PostQueuedCompletionStatus(handle->loop->iocp,
                                  req->u.io.overlapped.InternalHigh,
                                  0,
                                  &req->u.io.overlapped)) {
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");
  }
}


static void uv_tcp_queue_accept(uv_tcp_t* handle, uv_tcp_accept_t* req) {
  uv_loop_t* loop = handle->loop;
  BOOL success;
  DWORD bytes;
  SOCKET accept_socket;
  short family;

  assert(handle->flags & UV_HANDLE_LISTENING);
  assert(req->accept_socket == INVALID_SOCKET);

  /* choose family and extension function */
  if (handle->flags & UV_HANDLE_IPV6) {
    family = AF_INET6;
  } else {
    family = AF_INET;
  }

  /* Open a socket for the accepted connection. */
  accept_socket = socket(family, SOCK_STREAM, 0);
  if (accept_socket == INVALID_SOCKET) {
    SET_REQ_ERROR(req, WSAGetLastError());
    uv_insert_pending_req(loop, (uv_req_t*)req);
    handle->reqs_pending++;
    return;
  }

  /** 子进程不可继承 **/
  if (!SetHandleInformation((HANDLE) accept_socket, HANDLE_FLAG_INHERIT, 0)) {
    SET_REQ_ERROR(req, GetLastError());
    uv_insert_pending_req(loop, (uv_req_t*)req);
    handle->reqs_pending++;
    closesocket(accept_socket);
    return;
  }

  /* Prepare the overlapped structure. */
  memset(&(req->u.io.overlapped), 0, sizeof(req->u.io.overlapped));
  if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
    assert(req->event_handle != NULL);
    req->u.io.overlapped.hEvent = (HANDLE) ((ULONG_PTR) req->event_handle | 1);
  }

  /** 投递 **/
  success = handle->tcp.serv.func_acceptex(handle->socket,
                                          accept_socket,
                                          (void*)req->accept_buffer,
                                          0,
                                          sizeof(struct sockaddr_storage),
                                          sizeof(struct sockaddr_storage),
                                          &bytes,
                                          &req->u.io.overlapped);
  /** BOOL返回值 **/

  if (UV_SUCCEEDED_WITHOUT_IOCP(success)) {
    /* Process the req without IOCP. */
    req->accept_socket = accept_socket;
    handle->reqs_pending++;
    uv_insert_pending_req(loop, (uv_req_t*)req);
  } else if (UV_SUCCEEDED_WITH_IOCP(success)) { /** TURE || success == ERROR_IO_PENDING **/
    /* The req will be processed with IOCP. */
    req->accept_socket = accept_socket;
    handle->reqs_pending++;
    if (handle->flags & UV_HANDLE_EMULATE_IOCP &&
        req->wait_handle == INVALID_HANDLE_VALUE &&
        !RegisterWaitForSingleObject(&req->wait_handle,
          req->event_handle, post_completion, (void*) req,
          INFINITE, WT_EXECUTEINWAITTHREAD)) {
      SET_REQ_ERROR(req, GetLastError());
      /** 插入pending队列 **/
      uv_insert_pending_req(loop, (uv_req_t*)req);
    }
  } else { /** error **/
    /* Make this req pending reporting an error. */
    SET_REQ_ERROR(req, WSAGetLastError());
    uv_insert_pending_req(loop, (uv_req_t*)req);
    handle->reqs_pending++;
    /* Destroy the preallocated client socket. */
    closesocket(accept_socket);
    /* Destroy the event handle */
    if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
      CloseHandle(req->event_handle);
      req->event_handle = NULL;
    }
  }
}


static void uv_tcp_queue_read(uv_loop_t* loop, uv_tcp_t* handle) {
  uv_read_t* req;
  uv_buf_t buf;
  int result;
  DWORD bytes, flags;

  assert(handle->flags & UV_HANDLE_READING);
  assert(!(handle->flags & UV_HANDLE_READ_PENDING));

  req = &handle->read_req;
  memset(&req->u.io.overlapped, 0, sizeof(req->u.io.overlapped));

  /*
   * Preallocate a read buffer if the number of active streams is below
   * the threshold.
  */
  /** 连接数过多时, 投递空buf防止占用过多内存 **/
  if (loop->active_tcp_streams < uv_active_tcp_streams_threshold) {
    handle->flags &= ~UV_HANDLE_ZERO_READ;
    handle->tcp.conn.read_buffer = uv_buf_init(NULL, 0);
    handle->alloc_cb((uv_handle_t*) handle, 65536, &handle->tcp.conn.read_buffer);
    if (handle->tcp.conn.read_buffer.base == NULL ||
        handle->tcp.conn.read_buffer.len == 0) {
      handle->read_cb((uv_stream_t*) handle, UV_ENOBUFS, &handle->tcp.conn.read_buffer);
      return;
    }
    assert(handle->tcp.conn.read_buffer.base != NULL);
    buf = handle->tcp.conn.read_buffer;
  } else {
    handle->flags |= UV_HANDLE_ZERO_READ;
    buf.base = (char*) &uv_zero_;
    buf.len = 0;
  }

  /* Prepare the overlapped structure. */
  memset(&(req->u.io.overlapped), 0, sizeof(req->u.io.overlapped));
  if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
    assert(req->event_handle != NULL);
    req->u.io.overlapped.hEvent = (HANDLE) ((ULONG_PTR) req->event_handle | 1);
  }

  flags = 0;
  result = WSARecv(handle->socket,
                   (WSABUF*)&buf,
                   1,
                   &bytes,
                   &flags,
                   &req->u.io.overlapped,
                   NULL);

  if (UV_SUCCEEDED_WITHOUT_IOCP(result == 0)) {
    /* Process the req without IOCP. */
    handle->flags |= UV_HANDLE_READ_PENDING;
    req->u.io.overlapped.InternalHigh = bytes;
    handle->reqs_pending++;
    uv_insert_pending_req(loop, (uv_req_t*)req);
  } else if (UV_SUCCEEDED_WITH_IOCP(result == 0)) {
    /* The req will be processed with IOCP. */
    handle->flags |= UV_HANDLE_READ_PENDING;
    handle->reqs_pending++;
    if (handle->flags & UV_HANDLE_EMULATE_IOCP &&
        req->wait_handle == INVALID_HANDLE_VALUE &&
        !RegisterWaitForSingleObject(&req->wait_handle,
          req->event_handle, post_completion, (void*) req,
          INFINITE, WT_EXECUTEINWAITTHREAD)) {
      SET_REQ_ERROR(req, GetLastError());
      uv_insert_pending_req(loop, (uv_req_t*)req);
    }
  } else {
    /* Make this req pending reporting an error. */
    SET_REQ_ERROR(req, WSAGetLastError());
    uv_insert_pending_req(loop, (uv_req_t*)req);
    handle->reqs_pending++;
  }
}


int uv_tcp_close_reset(uv_tcp_t* handle, uv_close_cb close_cb) {
  struct linger l = { 1, 0 };

  /* Disallow setting SO_LINGER to zero due to some platform inconsistencies */
  if (handle->flags & UV_HANDLE_SHUTTING)
    return UV_EINVAL;

  if (0 != setsockopt(handle->socket, SOL_SOCKET, SO_LINGER, (const char*)&l, sizeof(l)))
    return uv_translate_sys_error(WSAGetLastError());

  uv_close((uv_handle_t*) handle, close_cb);
  return 0;
}


int uv_tcp_listen(uv_tcp_t* handle, int backlog, uv_connection_cb cb) {
  unsigned int i, simultaneous_accepts;
  uv_tcp_accept_t* req;
  int err;

  assert(backlog > 0);

  if (handle->flags & UV_HANDLE_LISTENING) {
    handle->stream.serv.connection_cb = cb;
  }

  if (handle->flags & UV_HANDLE_READING) {
    return WSAEISCONN;
  }

  /** 有未处理的错误 **/
  if (handle->delayed_error) {
    return handle->delayed_error;
  }

  if (!(handle->flags & UV_HANDLE_BOUND)) {
    /** 绑定ADDR_ANY:0, 由内核随机一个端口 **/
    err = uv_tcp_try_bind(handle,
                          (const struct sockaddr*) &uv_addr_ip4_any_,
                          sizeof(uv_addr_ip4_any_),
                          0);
    if (err)
      return err;
    if (handle->delayed_error)
      return handle->delayed_error;
  }

  /** 获取acceptex指针 **/
  if (!handle->tcp.serv.func_acceptex) {
    if (!uv_get_acceptex_function(handle->socket, &handle->tcp.serv.func_acceptex)) {
      return WSAEAFNOSUPPORT;
    }
  }

  /** listen **/
  if (!(handle->flags & UV_HANDLE_SHARED_TCP_SOCKET) &&
      listen(handle->socket, backlog) == SOCKET_ERROR) {
    return WSAGetLastError();
  }

  handle->flags |= UV_HANDLE_LISTENING;
  handle->stream.serv.connection_cb = cb;
  /** 增加计数 **/
  INCREASE_ACTIVE_COUNT(loop, handle);

  simultaneous_accepts = handle->flags & UV_HANDLE_TCP_SINGLE_ACCEPT ? 1
    : uv_simultaneous_server_accepts;

  if (handle->tcp.serv.accept_reqs == NULL) {
    /** 为accept_reqs分配空间, uv_tcp_endgame中释放 **/
    handle->tcp.serv.accept_reqs =
      uv__malloc(uv_simultaneous_server_accepts * sizeof(uv_tcp_accept_t));
    if (!handle->tcp.serv.accept_reqs) {
      uv_fatal_error(ERROR_OUTOFMEMORY, "uv__malloc");
    }

    for (i = 0; i < simultaneous_accepts; i++) {
      /** 初始化accept_req **/
      req = &handle->tcp.serv.accept_reqs[i];
      UV_REQ_INIT(req, UV_ACCEPT);
      req->accept_socket = INVALID_SOCKET;
      req->data = handle;

      req->wait_handle = INVALID_HANDLE_VALUE;
      /** 模拟iocp **/
      if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
        req->event_handle = CreateEvent(NULL, 0, 0, NULL);
        if (req->event_handle == NULL) {
          uv_fatal_error(GetLastError(), "CreateEvent");
        }
      } else {
        req->event_handle = NULL;
      }

      /** 投递accept **/
      uv_tcp_queue_accept(handle, req);
    }

    /* Initialize other unused requests too, because uv_tcp_endgame doesn't
     * know how many requests were initialized, so it will try to clean up
     * {uv_simultaneous_server_accepts} requests. */
    /** 初始化未使用的部分 **/
    for (i = simultaneous_accepts; i < uv_simultaneous_server_accepts; i++) {
      req = &handle->tcp.serv.accept_reqs[i];
      UV_REQ_INIT(req, UV_ACCEPT);
      req->accept_socket = INVALID_SOCKET;
      req->data = handle;
      req->wait_handle = INVALID_HANDLE_VALUE;
      req->event_handle = NULL;
    }
  }

  return 0;
}


int uv_tcp_accept(uv_tcp_t* server, uv_tcp_t* client) {
  uv_loop_t* loop = server->loop;
  int err = 0;
  int family;

  /** 获取pending_accepts队首 **/
  uv_tcp_accept_t* req = server->tcp.serv.pending_accepts;

  if (!req) {
    /* No valid connections found, so we error out. */
    return WSAEWOULDBLOCK;
  }

  /** 未连接 **/
  if (req->accept_socket == INVALID_SOCKET) {
    return WSAENOTCONN;
  }

  if (server->flags & UV_HANDLE_IPV6) {
    family = AF_INET6;
  } else {
    family = AF_INET;
  }

  /** 将socket关联到handle, nonblocking, cloexec, 关联iocp， 并设置套接字选项 **/
  err = uv_tcp_set_socket(client->loop,
                          client,
                          req->accept_socket,
                          family,
                          0);
  if (err) {
    closesocket(req->accept_socket);
  } else {
    uv_connection_init((uv_stream_t*) client);
    /* AcceptEx() implicitly binds the accepted socket. */
    client->flags |= UV_HANDLE_BOUND | UV_HANDLE_READABLE | UV_HANDLE_WRITABLE;
  }

  /** 将节点从pending_accepts中移除, 初始化重新投递 **/
  server->tcp.serv.pending_accepts = req->next_pending;
  req->next_pending = NULL;
  req->accept_socket = INVALID_SOCKET;

  /** server没关闭则继续投递 **/
  if (!(server->flags & UV_HANDLE_CLOSING)) {
    if (!(server->flags & UV_HANDLE_TCP_ACCEPT_STATE_CHANGING)) {
      uv_tcp_queue_accept(server, req);
    } else {
      /** 正在改变accept方式, 从连续accept切换到单次accept **/
      /* We better be switching to a single pending accept. */
      assert(server->flags & UV_HANDLE_TCP_SINGLE_ACCEPT);

      server->tcp.serv.processed_accepts++;

      /** 待所有pending_accepts被uv_accept, 切换到单次accept **/
      if (server->tcp.serv.processed_accepts >= uv_simultaneous_server_accepts) {
        server->tcp.serv.processed_accepts = 0;
        /*
         * All previously queued accept requests are now processed.
         * We now switch to queueing just a single accept.
         */
        uv_tcp_queue_accept(server, &server->tcp.serv.accept_reqs[0]);
        server->flags &= ~UV_HANDLE_TCP_ACCEPT_STATE_CHANGING;
        server->flags |= UV_HANDLE_TCP_SINGLE_ACCEPT;
      }
    }
  }

  loop->active_tcp_streams++;

  return err;
}


int uv_tcp_read_start(uv_tcp_t* handle, uv_alloc_cb alloc_cb,
    uv_read_cb read_cb) {
  uv_loop_t* loop = handle->loop;

  handle->flags |= UV_HANDLE_READING; /** flowing **/
  handle->read_cb = read_cb;
  handle->alloc_cb = alloc_cb;
  INCREASE_ACTIVE_COUNT(loop, handle);

  /**  **/
  if (!(handle->flags & UV_HANDLE_READ_PENDING)) {
    if (handle->flags & UV_HANDLE_EMULATE_IOCP &&
        handle->read_req.event_handle == NULL) {
      handle->read_req.event_handle = CreateEvent(NULL, 0, 0, NULL);
      if (handle->read_req.event_handle == NULL) {
        uv_fatal_error(GetLastError(), "CreateEvent");
      }
    }
    /** 投递请求 **/
    uv_tcp_queue_read(loop, handle);
  }

  return 0;
}


static int uv_tcp_try_connect(uv_connect_t* req,
                              uv_tcp_t* handle,
                              const struct sockaddr* addr,
                              unsigned int addrlen,
                              uv_connect_cb cb) {
  uv_loop_t* loop = handle->loop;
  const struct sockaddr* bind_addr;
  struct sockaddr_storage converted;
  BOOL success;
  DWORD bytes;
  int err;

  err = uv__convert_to_localhost_if_unspecified(addr, &converted);
  if (err)
    return err;

  if (handle->delayed_error) {
    return handle->delayed_error;
  }

  if (!(handle->flags & UV_HANDLE_BOUND)) {
    if (addrlen == sizeof(uv_addr_ip4_any_)) {
      bind_addr = (const struct sockaddr*) &uv_addr_ip4_any_;
    } else if (addrlen == sizeof(uv_addr_ip6_any_)) {
      bind_addr = (const struct sockaddr*) &uv_addr_ip6_any_;
    } else {
      abort();
    }
    err = uv_tcp_try_bind(handle, bind_addr, addrlen, 0);
    if (err)
      return err;
    if (handle->delayed_error)
      return handle->delayed_error;
  }

  if (!handle->tcp.conn.func_connectex) {
    if (!uv_get_connectex_function(handle->socket, &handle->tcp.conn.func_connectex)) {
      return WSAEAFNOSUPPORT;
    }
  }

  UV_REQ_INIT(req, UV_CONNECT);
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;
  memset(&req->u.io.overlapped, 0, sizeof(req->u.io.overlapped));

  success = handle->tcp.conn.func_connectex(handle->socket,
                                            (const struct sockaddr*) &converted,
                                            addrlen,
                                            NULL,
                                            0,
                                            &bytes,
                                            &req->u.io.overlapped);

  if (UV_SUCCEEDED_WITHOUT_IOCP(success)) {
    /* Process the req without IOCP. */
    handle->reqs_pending++;
    REGISTER_HANDLE_REQ(loop, handle, req);
    uv_insert_pending_req(loop, (uv_req_t*)req);
  } else if (UV_SUCCEEDED_WITH_IOCP(success)) {
    /* The req will be processed with IOCP. */
    handle->reqs_pending++;
    REGISTER_HANDLE_REQ(loop, handle, req);
  } else {
    return WSAGetLastError();
  }

  return 0;
}


int uv_tcp_getsockname(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {

  return uv__getsockpeername((const uv_handle_t*) handle,
                             getsockname,
                             name,
                             namelen,
                             handle->delayed_error);
}


int uv_tcp_getpeername(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {

  return uv__getsockpeername((const uv_handle_t*) handle,
                             getpeername,
                             name,
                             namelen,
                             handle->delayed_error);
}


int uv_tcp_write(uv_loop_t* loop,
                 uv_write_t* req,
                 uv_tcp_t* handle,
                 const uv_buf_t bufs[],
                 unsigned int nbufs,
                 uv_write_cb cb) {
  int result;
  DWORD bytes;

  UV_REQ_INIT(req, UV_WRITE);
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;

  /** 初始化重叠结构 **/
  memset(&(req->u.io.overlapped), 0, sizeof(req->u.io.overlapped));
  if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
    req->event_handle = CreateEvent(NULL, 0, 0, NULL);
    if (req->event_handle == NULL) {
      uv_fatal_error(GetLastError(), "CreateEvent");
    }
    req->u.io.overlapped.hEvent = (HANDLE) ((ULONG_PTR) req->event_handle | 1);
    req->wait_handle = INVALID_HANDLE_VALUE;
  }

  /** 投递 **/
  result = WSASend(handle->socket,
                   (WSABUF*) bufs,
                   nbufs,
                   &bytes,
                   0,
                   &req->u.io.overlapped,
                   NULL);

  if (UV_SUCCEEDED_WITHOUT_IOCP(result == 0)) {
    /* Request completed immediately. */
    req->u.io.queued_bytes = 0;
    handle->reqs_pending++;
    handle->stream.conn.write_reqs_pending++;
    REGISTER_HANDLE_REQ(loop, handle, req);
    uv_insert_pending_req(loop, (uv_req_t*) req);
  } else if (UV_SUCCEEDED_WITH_IOCP(result == 0)) { /** (result == 0) || (GetLastError() == ERROR_IO_PENDING) **/
    /* Request queued by the kernel. */
    /** 投递完成 **/
    req->u.io.queued_bytes = uv__count_bufs(bufs, nbufs);
    handle->reqs_pending++;
    handle->stream.conn.write_reqs_pending++;
    REGISTER_HANDLE_REQ(loop, handle, req);
    handle->write_queue_size += req->u.io.queued_bytes;
    if (handle->flags & UV_HANDLE_EMULATE_IOCP &&
        !RegisterWaitForSingleObject(&req->wait_handle,
          req->event_handle, post_write_completion, (void*) req,
          INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE)) {
      SET_REQ_ERROR(req, GetLastError());
      uv_insert_pending_req(loop, (uv_req_t*)req);
    }
  } else {
    /* Send failed due to an error, report it later */
    req->u.io.queued_bytes = 0;
    handle->reqs_pending++;
    handle->stream.conn.write_reqs_pending++;
    REGISTER_HANDLE_REQ(loop, handle, req);
    SET_REQ_ERROR(req, WSAGetLastError());
    uv_insert_pending_req(loop, (uv_req_t*) req);
  }

  return 0;
}


int uv__tcp_try_write(uv_tcp_t* handle,
                     const uv_buf_t bufs[],
                     unsigned int nbufs) {
  int result;
  DWORD bytes;

  if (handle->stream.conn.write_reqs_pending > 0)
    return UV_EAGAIN;

  result = WSASend(handle->socket,
                   (WSABUF*) bufs,
                   nbufs,
                   &bytes,
                   0,
                   NULL,
                   NULL);

  if (result == SOCKET_ERROR)
    return uv_translate_sys_error(WSAGetLastError());
  else
    return bytes;
}


void uv_process_tcp_read_req(uv_loop_t* loop, uv_tcp_t* handle,
    uv_req_t* req) {
  DWORD bytes, flags, err;
  uv_buf_t buf;
  int count;

  assert(handle->type == UV_TCP);

  handle->flags &= ~UV_HANDLE_READ_PENDING;

  if (!REQ_SUCCESS(req)) {
    /* An error occurred doing the read. */
    if ((handle->flags & UV_HANDLE_READING) ||
        !(handle->flags & UV_HANDLE_ZERO_READ)) {
      handle->flags &= ~UV_HANDLE_READING;
      DECREASE_ACTIVE_COUNT(loop, handle);
      buf = (handle->flags & UV_HANDLE_ZERO_READ) ?
            uv_buf_init(NULL, 0) : handle->tcp.conn.read_buffer;

      err = GET_REQ_SOCK_ERROR(req);

      if (err == WSAECONNABORTED) {
        /* Turn WSAECONNABORTED into UV_ECONNRESET to be consistent with Unix.
         */
        err = WSAECONNRESET;
      }

      handle->read_cb((uv_stream_t*)handle,
                      uv_translate_sys_error(err),
                      &buf);
    }
  } else {
    if (!(handle->flags & UV_HANDLE_ZERO_READ)) {
      /* The read was done with a non-zero buffer length. */
      if (req->u.io.overlapped.InternalHigh > 0) {
        /* Successful read */
        handle->read_cb((uv_stream_t*)handle,
                        req->u.io.overlapped.InternalHigh,
                        &handle->tcp.conn.read_buffer);
        /* Read again only if bytes == buf.len */
        if (req->u.io.overlapped.InternalHigh < handle->tcp.conn.read_buffer.len) {
          goto done;
        }
      } else {
        /* Connection closed */
        if (handle->flags & UV_HANDLE_READING) {
          handle->flags &= ~UV_HANDLE_READING;
          DECREASE_ACTIVE_COUNT(loop, handle);
        }
        handle->flags &= ~UV_HANDLE_READABLE;

        buf.base = 0;
        buf.len = 0;
        handle->read_cb((uv_stream_t*)handle, UV_EOF, &handle->tcp.conn.read_buffer);
        goto done;
      }
    }

    /* Do nonblocking reads until the buffer is empty */
    count = 32;
    while ((handle->flags & UV_HANDLE_READING) && (count-- > 0)) {
      buf = uv_buf_init(NULL, 0);
      handle->alloc_cb((uv_handle_t*) handle, 65536, &buf);
      if (buf.base == NULL || buf.len == 0) {
        handle->read_cb((uv_stream_t*) handle, UV_ENOBUFS, &buf);
        break;
      }
      assert(buf.base != NULL);

      flags = 0;
      if (WSARecv(handle->socket,
                  (WSABUF*)&buf,
                  1,
                  &bytes,
                  &flags,
                  NULL,
                  NULL) != SOCKET_ERROR) {
        if (bytes > 0) {
          /* Successful read */
          handle->read_cb((uv_stream_t*)handle, bytes, &buf);
          /* Read again only if bytes == buf.len */
          if (bytes < buf.len) {
            break;
          }
        } else {
          /* Connection closed */
          handle->flags &= ~(UV_HANDLE_READING | UV_HANDLE_READABLE);
          DECREASE_ACTIVE_COUNT(loop, handle);

          handle->read_cb((uv_stream_t*)handle, UV_EOF, &buf);
          break;
        }
      } else {
        err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
          /* Read buffer was completely empty, report a 0-byte read. */
          handle->read_cb((uv_stream_t*)handle, 0, &buf);
        } else {
          /* Ouch! serious error. */
          handle->flags &= ~UV_HANDLE_READING;
          DECREASE_ACTIVE_COUNT(loop, handle);

          if (err == WSAECONNABORTED) {
            /* Turn WSAECONNABORTED into UV_ECONNRESET to be consistent with
             * Unix. */
            err = WSAECONNRESET;
          }

          handle->read_cb((uv_stream_t*)handle,
                          uv_translate_sys_error(err),
                          &buf);
        }
        break;
      }
    }

done:
    /* Post another read if still reading and not closing. */
    if ((handle->flags & UV_HANDLE_READING) &&
        !(handle->flags & UV_HANDLE_READ_PENDING)) {
      uv_tcp_queue_read(loop, handle);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_tcp_write_req(uv_loop_t* loop, uv_tcp_t* handle,
    uv_write_t* req) {
  int err;

  assert(handle->type == UV_TCP);

  assert(handle->write_queue_size >= req->u.io.queued_bytes);
  handle->write_queue_size -= req->u.io.queued_bytes;

  UNREGISTER_HANDLE_REQ(loop, handle, req);

  if (handle->flags & UV_HANDLE_EMULATE_IOCP) {
    if (req->wait_handle != INVALID_HANDLE_VALUE) {
      UnregisterWait(req->wait_handle);
      req->wait_handle = INVALID_HANDLE_VALUE;
    }
    if (req->event_handle != NULL) {
      CloseHandle(req->event_handle);
      req->event_handle = NULL;
    }
  }

  if (req->cb) {
    err = uv_translate_sys_error(GET_REQ_SOCK_ERROR(req));
    if (err == UV_ECONNABORTED) {
      /* use UV_ECANCELED for consistency with Unix */
      err = UV_ECANCELED;
    }
    req->cb(req, err);
  }

  handle->stream.conn.write_reqs_pending--;
  if (handle->stream.conn.shutdown_req != NULL &&
      handle->stream.conn.write_reqs_pending == 0) {
    uv_want_endgame(loop, (uv_handle_t*)handle);
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_tcp_accept_req(uv_loop_t* loop, uv_tcp_t* handle,
    uv_req_t* raw_req) {
  uv_tcp_accept_t* req = (uv_tcp_accept_t*) raw_req;
  int err;

  assert(handle->type == UV_TCP);

  /* If handle->accepted_socket is not a valid socket, then uv_queue_accept
   * must have failed. This is a serious error. We stop accepting connections
   * and report this error to the connection callback. */
  /** 在之前的操作中发生了错误 **/
  if (req->accept_socket == INVALID_SOCKET) {
    if (handle->flags & UV_HANDLE_LISTENING) {
      handle->flags &= ~UV_HANDLE_LISTENING;
      DECREASE_ACTIVE_COUNT(loop, handle);
      if (handle->stream.serv.connection_cb) {
        err = GET_REQ_SOCK_ERROR(req);
        handle->stream.serv.connection_cb((uv_stream_t*)handle,
                                      uv_translate_sys_error(err));
      }
    }
  } else if (REQ_SUCCESS(req) && /** (req)->u.io.overlapped.Internal **/
      /** 继承监听套接字的属性 **/
      setsockopt(req->accept_socket,
                  SOL_SOCKET,
                  SO_UPDATE_ACCEPT_CONTEXT,
                  (char*)&handle->socket,
                  sizeof(handle->socket)) == 0) {
    /** 将req插入pending链表, 待uv_accept获取 **/
    req->next_pending = handle->tcp.serv.pending_accepts;
    handle->tcp.serv.pending_accepts = req;

    /* Accept and SO_UPDATE_ACCEPT_CONTEXT were successful. */
    /** 回调 **/
    if (handle->stream.serv.connection_cb) {
      handle->stream.serv.connection_cb((uv_stream_t*)handle, 0);
    }
  } else {
    /**
     * 与服务器接受的套接字有关的错误将被忽略, 因为服务器套接字可能仍然正常.
     * 如果服务器套接字损坏, uv_queue_accept将对其进行检测.
     **/
    closesocket(req->accept_socket);
    req->accept_socket = INVALID_SOCKET;
    if (handle->flags & UV_HANDLE_LISTENING) {
      uv_tcp_queue_accept(handle, req);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_tcp_connect_req(uv_loop_t* loop, uv_tcp_t* handle,
    uv_connect_t* req) {
  int err;

  assert(handle->type == UV_TCP);

  UNREGISTER_HANDLE_REQ(loop, handle, req);

  err = 0;
  if (REQ_SUCCESS(req)) {
    if (handle->flags & UV_HANDLE_CLOSING) {
      /* use UV_ECANCELED for consistency with Unix */
      err = ERROR_OPERATION_ABORTED;
    } else if (setsockopt(handle->socket,
                          SOL_SOCKET,
                          SO_UPDATE_CONNECT_CONTEXT,
                          NULL,
                          0) == 0) {
      uv_connection_init((uv_stream_t*)handle);
      handle->flags |= UV_HANDLE_READABLE | UV_HANDLE_WRITABLE;
      loop->active_tcp_streams++;
    } else {
      err = WSAGetLastError();
    }
  } else {
    err = GET_REQ_SOCK_ERROR(req);
  }
  req->cb(req, uv_translate_sys_error(err));

  DECREASE_PENDING_REQ_COUNT(handle);
}


int uv__tcp_xfer_export(uv_tcp_t* handle,
                        int target_pid,
                        uv__ipc_socket_xfer_type_t* xfer_type,
                        uv__ipc_socket_xfer_info_t* xfer_info) {
  if (handle->flags & UV_HANDLE_CONNECTION) {
    *xfer_type = UV__IPC_SOCKET_XFER_TCP_CONNECTION;
  } else {
    *xfer_type = UV__IPC_SOCKET_XFER_TCP_SERVER;
    /* We're about to share the socket with another process. Because this is a
     * listening socket, we assume that the other process will be accepting
     * connections on it. Thus, before sharing the socket with another process,
     * we call listen here in the parent process. */
    if (!(handle->flags & UV_HANDLE_LISTENING)) {
      if (!(handle->flags & UV_HANDLE_BOUND)) {
        return ERROR_NOT_SUPPORTED;
      }
      if (handle->delayed_error == 0 &&
          listen(handle->socket, SOMAXCONN) == SOCKET_ERROR) {
        handle->delayed_error = WSAGetLastError();
      }
    }
  }

  if (WSADuplicateSocketW(handle->socket, target_pid, &xfer_info->socket_info))
    return WSAGetLastError();
  xfer_info->delayed_error = handle->delayed_error;

  /* Mark the local copy of the handle as 'shared' so we behave in a way that's
   * friendly to the process(es) that we share the socket with. */
  handle->flags |= UV_HANDLE_SHARED_TCP_SOCKET;

  return 0;
}


int uv__tcp_xfer_import(uv_tcp_t* tcp,
                        uv__ipc_socket_xfer_type_t xfer_type,
                        uv__ipc_socket_xfer_info_t* xfer_info) {
  int err;
  SOCKET socket;

  assert(xfer_type == UV__IPC_SOCKET_XFER_TCP_SERVER ||
         xfer_type == UV__IPC_SOCKET_XFER_TCP_CONNECTION);

  socket = WSASocketW(FROM_PROTOCOL_INFO,
                      FROM_PROTOCOL_INFO,
                      FROM_PROTOCOL_INFO,
                      &xfer_info->socket_info,
                      0,
                      WSA_FLAG_OVERLAPPED);

  if (socket == INVALID_SOCKET) {
    return WSAGetLastError();
  }

  /** 将socket关联到handle, nonblocking, cloexec, 关联iocp， 并设置套接字选项 **/
  err = uv_tcp_set_socket(
      tcp->loop, tcp, socket, xfer_info->socket_info.iAddressFamily, 1);
  if (err) {
    closesocket(socket);
    return err;
  }

  tcp->delayed_error = xfer_info->delayed_error;
  tcp->flags |= UV_HANDLE_BOUND | UV_HANDLE_SHARED_TCP_SOCKET;

  if (xfer_type == UV__IPC_SOCKET_XFER_TCP_CONNECTION) {
    uv_connection_init((uv_stream_t*)tcp);
    tcp->flags |= UV_HANDLE_READABLE | UV_HANDLE_WRITABLE;
  }

  tcp->loop->active_tcp_streams++;
  return 0;
}


int uv_tcp_nodelay(uv_tcp_t* handle, int enable) {
  int err;

  if (handle->socket != INVALID_SOCKET) {
    err = uv__tcp_nodelay(handle, handle->socket, enable);
    if (err)
      return err;
  }

  if (enable) {
    handle->flags |= UV_HANDLE_TCP_NODELAY;
  } else {
    handle->flags &= ~UV_HANDLE_TCP_NODELAY;
  }

  return 0;
}


int uv_tcp_keepalive(uv_tcp_t* handle, int enable, unsigned int delay) {
  int err;

  if (handle->socket != INVALID_SOCKET) {
    err = uv__tcp_keepalive(handle, handle->socket, enable, delay);
    if (err)
      return err;
  }

  if (enable) {
    handle->flags |= UV_HANDLE_TCP_KEEPALIVE;
  } else {
    handle->flags &= ~UV_HANDLE_TCP_KEEPALIVE;
  }

  /* TODO: Store delay if handle->socket isn't created yet. */

  return 0;
}


int uv_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable) {
  if (handle->flags & UV_HANDLE_CONNECTION) {
    return UV_EINVAL;
  }

  /* Check if we're already in the desired mode. */
  if ((enable && !(handle->flags & UV_HANDLE_TCP_SINGLE_ACCEPT)) ||
      (!enable && handle->flags & UV_HANDLE_TCP_SINGLE_ACCEPT)) {
    return 0;
  }

  /** 不允许从单次accept切换到连续accept **/
  if (enable) {
    return UV_ENOTSUP;
  }

  /* Check if we're in a middle of changing the number of pending accepts. */
  if (handle->flags & UV_HANDLE_TCP_ACCEPT_STATE_CHANGING) {
    return 0;
  }

  handle->flags |= UV_HANDLE_TCP_SINGLE_ACCEPT;

  /* Flip the changing flag if we have already queued multiple accepts. */
  if (handle->flags & UV_HANDLE_LISTENING) {
    handle->flags |= UV_HANDLE_TCP_ACCEPT_STATE_CHANGING;
  }

  return 0;
}


static int uv_tcp_try_cancel_io(uv_tcp_t* tcp) {
  SOCKET socket = tcp->socket;
  int non_ifs_lsp;

  /* Check if we have any non-IFS LSPs stacked on top of TCP */
  non_ifs_lsp = (tcp->flags & UV_HANDLE_IPV6) ? uv_tcp_non_ifs_lsp_ipv6 :
                                                uv_tcp_non_ifs_lsp_ipv4;

  /* If there are non-ifs LSPs then try to obtain a base handle for the socket.
   * This will always fail on Windows XP/3k. */
  if (non_ifs_lsp) {
    DWORD bytes;
    if (WSAIoctl(socket,
                 SIO_BASE_HANDLE,
                 NULL,
                 0,
                 &socket,
                 sizeof socket,
                 &bytes,
                 NULL,
                 NULL) != 0) {
      /* Failed. We can't do CancelIo. */
      return -1;
    }
  }

  assert(socket != 0 && socket != INVALID_SOCKET);

  if (!CancelIo((HANDLE) socket)) {
    return GetLastError();
  }

  /* It worked. */
  return 0;
}


void uv_tcp_close(uv_loop_t* loop, uv_tcp_t* tcp) {
  int close_socket = 1;

  if (tcp->flags & UV_HANDLE_READ_PENDING) {
    /* In order for winsock to do a graceful close there must not be any any
     * pending reads, or the socket must be shut down for writing */
    if (!(tcp->flags & UV_HANDLE_SHARED_TCP_SOCKET)) {
      /* Just do shutdown on non-shared sockets, which ensures graceful close. */
      shutdown(tcp->socket, SD_SEND);

    } else if (uv_tcp_try_cancel_io(tcp) == 0) {
      /* In case of a shared socket, we try to cancel all outstanding I/O,. If
       * that works, don't close the socket yet - wait for the read req to
       * return and close the socket in uv_tcp_endgame. */
      close_socket = 0;

    } else {
      /* When cancelling isn't possible - which could happen when an LSP is
       * present on an old Windows version, we will have to close the socket
       * with a read pending. That is not nice because trailing sent bytes may
       * not make it to the other side. */
    }

  } else if ((tcp->flags & UV_HANDLE_SHARED_TCP_SOCKET) &&
             tcp->tcp.serv.accept_reqs != NULL) {
    /* Under normal circumstances closesocket() will ensure that all pending
     * accept reqs are canceled. However, when the socket is shared the
     * presence of another reference to the socket in another process will keep
     * the accept reqs going, so we have to ensure that these are canceled. */
    if (uv_tcp_try_cancel_io(tcp) != 0) {
      /* When cancellation is not possible, there is another option: we can
       * close the incoming sockets, which will also cancel the accept
       * operations. However this is not cool because we might inadvertently
       * close a socket that just accepted a new connection, which will cause
       * the connection to be aborted. */
      unsigned int i;
      for (i = 0; i < uv_simultaneous_server_accepts; i++) {
        uv_tcp_accept_t* req = &tcp->tcp.serv.accept_reqs[i];
        if (req->accept_socket != INVALID_SOCKET &&
            !HasOverlappedIoCompleted(&req->u.io.overlapped)) {
          closesocket(req->accept_socket);
          req->accept_socket = INVALID_SOCKET;
        }
      }
    }
  }

  if (tcp->flags & UV_HANDLE_READING) {
    tcp->flags &= ~UV_HANDLE_READING;
    DECREASE_ACTIVE_COUNT(loop, tcp);
  }

  if (tcp->flags & UV_HANDLE_LISTENING) {
    tcp->flags &= ~UV_HANDLE_LISTENING;
    DECREASE_ACTIVE_COUNT(loop, tcp);
  }

  if (close_socket) {
    closesocket(tcp->socket);
    tcp->socket = INVALID_SOCKET;
    tcp->flags |= UV_HANDLE_TCP_SOCKET_CLOSED;
  }

  tcp->flags &= ~(UV_HANDLE_READABLE | UV_HANDLE_WRITABLE);
  uv__handle_closing(tcp);

  if (tcp->reqs_pending == 0) {
    uv_want_endgame(tcp->loop, (uv_handle_t*)tcp);
  }
}


int uv_tcp_open(uv_tcp_t* handle, uv_os_sock_t sock) {
  WSAPROTOCOL_INFOW protocol_info;
  int opt_len;
  int err;
  struct sockaddr_storage saddr;
  int saddr_len;

  /* Detect the address family of the socket. */
  opt_len = (int) sizeof protocol_info;
  if (getsockopt(sock,
                 SOL_SOCKET,
                 SO_PROTOCOL_INFOW,
                 (char*) &protocol_info,
                 &opt_len) == SOCKET_ERROR) {
    return uv_translate_sys_error(GetLastError());
  }

  /** 将socket关联到handle, nonblocking, cloexec, 关联iocp， 并设置套接字选项 **/
  err = uv_tcp_set_socket(handle->loop,
                          handle,
                          sock,
                          protocol_info.iAddressFamily,
                          1);
  if (err) {
    return uv_translate_sys_error(err);
  }

  /* Support already active socket. */
  saddr_len = sizeof(saddr);
  if (!uv_tcp_getsockname(handle, (struct sockaddr*) &saddr, &saddr_len)) {
    /* Socket is already bound. */
    handle->flags |= UV_HANDLE_BOUND;
    saddr_len = sizeof(saddr);
    if (!uv_tcp_getpeername(handle, (struct sockaddr*) &saddr, &saddr_len)) {
      /* Socket is already connected. */
      uv_connection_init((uv_stream_t*) handle);
      handle->flags |= UV_HANDLE_READABLE | UV_HANDLE_WRITABLE;
    }
  }

  return 0;
}


/* This function is an egress point, i.e. it returns libuv errors rather than
 * system errors.
 */
int uv__tcp_bind(uv_tcp_t* handle,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 unsigned int flags) {
  int err;

  err = uv_tcp_try_bind(handle, addr, addrlen, flags);
  if (err)
    return uv_translate_sys_error(err);

  return 0;
}


/* This function is an egress point, i.e. it returns libuv errors rather than
 * system errors.
 */
int uv__tcp_connect(uv_connect_t* req,
                    uv_tcp_t* handle,
                    const struct sockaddr* addr,
                    unsigned int addrlen,
                    uv_connect_cb cb) {
  int err;

  err = uv_tcp_try_connect(req, handle, addr, addrlen, cb);
  if (err)
    return uv_translate_sys_error(err);

  return 0;
}
