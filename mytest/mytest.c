//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <uv.h>
//#include "../src/uv-common.h"
//
//#define DEFAULT_PORT 7000
//#define DEFAULT_BACKLOG 128
//
//void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
//  buf->base = (char*) malloc(suggested_size);
//  buf->len = suggested_size;
//}
//
//void echo_write(uv_write_t *req, int status) {
//  if (status) {
//    fprintf(stderr, "[c]write error: %s\n", uv_strerror(status));
//  }
//  uv_close((uv_handle_t*) req->handle, NULL);
//  free(req);
//}
//
//void echo_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
//  if (nread < 0) {
//    if (nread != UV_EOF)
//      fprintf(stderr, "[s]read error: %s\n", uv_err_name(nread));
//    else
//      fprintf(stderr, "[s]read eof\n");
//    uv_handle_t *server = client->data;
//    uv_close((uv_handle_t*) client, NULL);
//    uv_close(server, NULL);
//  } else if (nread > 0) {
//    printf("[s]echo_read: %s\n",buf->base);
//  }
//
//  if (buf->base)
//    free(buf->base);
//}
//
//void on_new_connection(uv_stream_t *server, int status) {
//  uv_loop_t *loop = server->loop;
//  if (status < 0) {
//    fprintf(stderr, "[s]new connection error: %s\n", uv_strerror(status));
//    return;
//  }
//
//  uv_tcp_t *client = (uv_tcp_t*) malloc(sizeof(uv_tcp_t));
//  client->data = server;
//  uv_tcp_init(loop, client);
//  if (uv_accept(server, (uv_stream_t*) client) == 0) {
//    uv_read_start((uv_stream_t*) client, alloc_buffer, echo_read);
//  }
//  else {
//    uv_close((uv_handle_t*) client, NULL);
//  }
//  printf("[s]on new connection, status: %d\n", status);
//}
//
//void on_timeout (uv_timer_t *handle) {
//  uv_close((uv_handle_t *)handle,  NULL);
//  uv_tcp_t *server = (uv_tcp_t *)handle->data;
//  if (uv__is_closing((uv_handle_t *)handle->data))
//    return;
//  uv_close((uv_handle_t *)server, NULL);
//}
//
//void tcp_server (uv_loop_t *loop) {
//  /* server */
//  static uv_tcp_t server;
//  uv_tcp_init(loop, &server);
//
//  struct sockaddr_in addr;
//  uv_ip4_addr("0.0.0.0", DEFAULT_PORT, &addr);
//
//  uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
//  int r = uv_listen((uv_stream_t*) &server, DEFAULT_BACKLOG, on_new_connection);
//  if (r) {
//    fprintf(stderr, "Listen error %s\n", uv_strerror(r));
//    uv_close((uv_handle_t *)&server, NULL);
//    return;
//  }
//  printf("[s]listening... [addr=%s, port=%d]\n", "0.0.0.0", DEFAULT_PORT);
//
//  uv_timer_t *timer = (uv_timer_t *)malloc(sizeof(uv_timer_t));
//  timer->data = &server;
//  uv_timer_init(loop, timer);
//  uv_timer_start(timer, on_timeout, 3000, 0);
//}
//
//void on_connected (uv_connect_t *handle, int status) {
//  if (status < 0) {
//    fprintf(stderr, "[c]uv_tcp_connect error %s\n", uv_strerror(status));
//    return;
//  }
//  printf("[c]connected to server, status: %d\n", status);
//  uv_write_t *req = (uv_write_t *) malloc(sizeof(uv_write_t));
//  uv_buf_t buf = uv_buf_init("hello world", 14);
//  uv_write(req, (uv_stream_t *)handle->handle, &buf, 1, echo_write);
//}
//
//void tcp_client (uv_loop_t *loop) {
//  static uv_tcp_t client;
//  uv_tcp_init(loop, &client);
//  uv_tcp_keepalive(&client, 1, 10);
//
//  struct sockaddr_in addr;
//  uv_ip4_addr("127.0.0.1", DEFAULT_PORT, &addr);
//
//  uv_connect_t *connector = (uv_connect_t *)malloc(sizeof(uv_connect_t));
//  uv_tcp_connect(connector, &client, (const struct sockaddr*)&addr, on_connected);
//  printf("[c]connecting...\n");
//}
//
//#if 0
//void on_idle (uv_idle_t *handle) {
//  static int times = 0;
//  printf("idle: %d\n", times);
//  if (++times >= 4)
//    uv_close((uv_handle_t *)handle, NULL);
//}
//
//void on_check (uv_check_t *handle) {
//  static int times = 0;
//  printf("check: %d\n", times);
//  if (++times >= 3)
//    uv_close((uv_handle_t *)handle, NULL);
//}
//
//void on_prepare (uv_prepare_t *handle) {
//  static int times = 0;
//  printf("prepare: %d\n", times);
//  if (++times >= 3)
//    uv_close((uv_handle_t *)handle, NULL);
//}
//#endif
//
//#if 0
//void on_timer (uv_timer_t *handle) {
//  static int times = 0;
//  printf("on_timer: %d\n", times);
//  if (++times >= 10)
//    uv_close((uv_handle_t *)handle, NULL);
//}
//#endif
//
//int main () {
//  uv_loop_t *loop = uv_default_loop();
//
//#if 1
//  {
//    tcp_server(loop);
//    tcp_client(loop);
//    return uv_run(loop, UV_RUN_DEFAULT);
//  }
//#endif
//
//  /* timer */
//#if 0
//  uv_timer_t timer;
//  uv_timer_init(loop, &timer);
//  uv_timer_start(&timer, on_timer, 1000, 1000);
//#endif
//
//  /* task */
//#if 0
//  uv_prepare_t prepare;
//  uv_prepare_init(loop, &prepare);
//  uv_prepare_start(&prepare, on_prepare);
//
//  uv_check_t check;
//  uv_check_init(loop, &check);
//  uv_check_start(&check, on_check);
//
//  uv_idle_t idle;
//  uv_idle_init(loop, &idle);
//  uv_idle_start(&idle, on_idle);
//
//#endif
//  return uv_run(loop, UV_RUN_DEFAULT);
//}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

uv_loop_t *loop;

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

void free_write_req(uv_write_t *req) {
  write_req_t *wr = (write_req_t*) req;
  free(wr->buf.base);
  free(wr);
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

void echo_write(uv_write_t *req, int status) {
  if (status < 0) {
    fprintf(stderr, "Write error %s\n", uv_err_name(status));
  }
  free_write_req(req);
}

void echo_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
  if (nread > 0) {
    write_req_t *req = (write_req_t*) malloc(sizeof(write_req_t));
    req->buf = uv_buf_init(buf->base, nread);
    uv_write((uv_write_t*) req, client, &req->buf, 1, echo_write);
    return;
  }

  if (nread < 0) {
    if (nread != UV_EOF)
      fprintf(stderr, "Read error %s\n", uv_err_name(nread));
    uv_close((uv_handle_t*) client, NULL);
  }

  free(buf->base);
}

void on_new_connection(uv_stream_t *server, int status) {
  if (status == -1) {
    // error!
    return;
  }

  uv_pipe_t *client = (uv_pipe_t*) malloc(sizeof(uv_pipe_t));
  uv_pipe_init(loop, client, 0);
  if (uv_accept(server, (uv_stream_t*) client) == 0) {
    uv_read_start((uv_stream_t*) client, alloc_buffer, echo_read);
  }
  else {
    uv_close((uv_handle_t*) client, NULL);
  }
}

void remove_sock(int sig) {
  uv_fs_t req;
  uv_fs_unlink(loop, &req, "echo.sock", NULL);
  exit(0);
}

int main() {
  loop = uv_default_loop();

  uv_pipe_t server;
  uv_pipe_init(loop, &server, 0);

  signal(SIGINT, remove_sock);

  int r;
  if ((r = uv_pipe_bind(&server, "echo.sock"))) {
    fprintf(stderr, "Bind error %s\n", uv_err_name(r));
    return 1;
  }
  if ((r = uv_listen((uv_stream_t*) &server, 128, on_new_connection))) {
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 2;
  }
  return uv_run(loop, UV_RUN_DEFAULT);
}
