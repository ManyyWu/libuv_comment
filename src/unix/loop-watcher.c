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

#define UV_LOOP_WATCHER_DEFINE(name, type)                                    \
  int uv_##name##_init(uv_loop_t* loop, uv_##name##_t* handle) {              \
    uv__handle_init(loop, (uv_handle_t*)handle, UV_##type);                   \
    handle->name##_cb = NULL;                                                 \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##name##_start(uv_##name##_t* handle, uv_##name##_cb cb) {           \
    if (uv__is_active(handle)) return 0;                                      \
    if (cb == NULL) return UV_EINVAL;                                         \
    QUEUE_INSERT_HEAD(&handle->loop->name##_handles, &handle->queue);         \
    handle->name##_cb = cb;                                                   \
    uv__handle_start(handle);                                                 \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  int uv_##name##_stop(uv_##name##_t* handle) {                               \
    if (!uv__is_active(handle)) return 0;                                     \
    QUEUE_REMOVE(&handle->queue);                                             \
    uv__handle_stop(handle);                                                  \
    return 0;                                                                 \
  }                                                                           \
                                                                              \
  void uv__run_##name(uv_loop_t* loop) {                                      \
    uv_##name##_t* h;                                                         \
    QUEUE queue;                                                              \
    QUEUE* q;                                                                 \
    QUEUE_MOVE(&loop->name##_handles, &queue);                                \
    while (!QUEUE_EMPTY(&queue)) {                                            \
      q = QUEUE_HEAD(&queue);                                                 \
      h = QUEUE_DATA(q, uv_##name##_t, queue);                                \
      QUEUE_REMOVE(q);                                                        \
      QUEUE_INSERT_TAIL(&loop->name##_handles, q);                            \
      h->name##_cb(h);                                                        \
    }                                                                         \
  }                                                                           \
                                                                              \
  void uv__##name##_close(uv_##name##_t* handle) {                            \
    uv_##name##_stop(handle);                                                 \
  }

//UV_LOOP_WATCHER_DEFINE(prepare, PREPARE)
//UV_LOOP_WATCHER_DEFINE(check, CHECK)
//UV_LOOP_WATCHER_DEFINE(idle, IDLE)

int uv_prepare_init(uv_loop_t* loop, uv_prepare_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_PREPARE);
  handle->prepare_cb = NULL;
  return 0;
}

int uv_prepare_start(uv_prepare_t* handle, uv_prepare_cb cb) {
  if (uv__is_active(handle)) return 0;
  if (cb == NULL) return UV_EINVAL;
  QUEUE_INSERT_HEAD(&handle->loop->prepare_handles, &handle->queue);
  handle->prepare_cb = cb;
  uv__handle_start(handle);
  return 0;
}

int uv_prepare_stop(uv_prepare_t* handle) {
  if (!uv__is_active(handle)) return 0;
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  return 0;
}

void uv__run_prepare(uv_loop_t* loop) {
  uv_prepare_t* h;
  QUEUE queue;
  QUEUE* q;
  QUEUE_MOVE(&loop->prepare_handles, &queue);
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_prepare_t, queue);
    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->prepare_handles, q);
    h->prepare_cb(h);
  }
}

void uv__prepare_close(uv_prepare_t* handle) {
  uv_prepare_stop(handle);
}

int uv_check_init(uv_loop_t* loop, uv_check_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_CHECK);
  handle->check_cb = NULL;
  return 0;
}

int uv_check_start(uv_check_t* handle, uv_check_cb cb) {
  if (uv__is_active(handle)) return 0;
  if (cb == NULL) return UV_EINVAL;
  QUEUE_INSERT_HEAD(&handle->loop->check_handles, &handle->queue);
  handle->check_cb = cb;
  uv__handle_start(handle);
  return 0;
}

int uv_check_stop(uv_check_t* handle) {
  if (!uv__is_active(handle)) return 0;
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  return 0;
}

void uv__run_check(uv_loop_t* loop) {
  uv_check_t* h;
  QUEUE queue;
  QUEUE* q;
  QUEUE_MOVE(&loop->check_handles, &queue); /** 将check_queue从loop移除, 防止回调用修改队列 **/
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_check_t, queue);
    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->check_handles, q);/** 重新将节点插入check_queue **/
    h->check_cb(h);
  }
}

void uv__check_close(uv_check_t* handle) {
  uv_check_stop(handle);
}

int uv_idle_init(uv_loop_t* loop, uv_idle_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_IDLE);
    handle->idle_cb = NULL;
    return 0;
}

int uv_idle_start(uv_idle_t* handle, uv_idle_cb cb) {
  if (uv__is_active(handle)) return 0;
  if (cb == NULL) return UV_EINVAL;
  QUEUE_INSERT_HEAD(&handle->loop->idle_handles, &handle->queue);
  handle->idle_cb = cb;
  uv__handle_start(handle);
  return 0;
}

int uv_idle_stop(uv_idle_t* handle) {
  if (!uv__is_active(handle)) return 0;
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  return 0;
}

void uv__run_idle(uv_loop_t* loop) {
  uv_idle_t* h;
  QUEUE queue;
  QUEUE* q;
  QUEUE_MOVE(&loop->idle_handles, &queue);
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_idle_t, queue);
    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->idle_handles, q);
    h->idle_cb(h);
  }
}

void uv__idle_close(uv_idle_t* handle) {
  uv_idle_stop(handle);
}
