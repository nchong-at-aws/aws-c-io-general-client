/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/allocator.h>
#include <aws/common/clock.h>
#include <aws/common/device_random.h>
#include <aws/common/task_scheduler.h>
#include <aws/io/event_loop.h>
#include <aws/testing/aws_test_harness.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef NUM_THREADS
#define NUM_THREADS 2
#endif
#ifndef NUM_API_CALL_ROUNDS
#define NUM_API_CALL_ROUNDS 2
#endif
#ifndef NUM_API_CALLS
#define NUM_API_CALLS 3
#endif

struct client_args {
  struct aws_event_loop *event_loop;
  uint8_t choices[NUM_API_CALL_ROUNDS];
  struct aws_io_handle handle;
  struct aws_task task;
};

// bug#1
// if a task dereferences handle->additional_data
// https://github.com/awslabs/aws-c-io/pull/200

// bug#2
// if distinct threads concurrently call stop then they can corrupt the
// task_pre_queue in the Linux epoll event loop implementation prior to 
// https://github.com/awslabs/aws-c-io/pull/201

void on_event(struct aws_event_loop *event_loop, struct aws_io_handle *handle, int events, void *user_data) {
  assert(handle);
  (void)events;
  (void)user_data;
  assert(handle->additional_data); //< bug#1
}

void unsub_task(struct aws_task *task, void *user_data, enum aws_task_status status) {
  struct client_args *client_args = user_data;
  aws_event_loop_unsubscribe_from_io_events(client_args->event_loop, &client_args->handle);
}

void client(void *args) {
  assert(args);
  struct client_args *client_args = args;
  struct aws_event_loop *event_loop = client_args->event_loop;
  assert(event_loop);
  uint8_t *choices = client_args->choices;
  assert(choices);
  uint64_t time_nanos;
  int result;
  bool subscribed = false;
  for (int i=0; i<NUM_API_CALL_ROUNDS; ++i) {
#ifdef VERIFY
    choices[i] = nondet_int();
    assume(0 <= i && i < NUM_API_CALLS);
#endif
    switch(choices[i]) {
      case 0:
        result = aws_event_loop_stop(event_loop);  //#< bug#2
        assert(result == AWS_OP_SUCCESS);
        break;
      case 1:
        result = aws_event_loop_current_clock_time(event_loop, &time_nanos);
        assert(result == AWS_OP_SUCCESS);
        break;
      case 2:
        if (subscribed) break;
        subscribed = true;
        aws_event_loop_subscribe_to_io_events(event_loop,
          &client_args->handle,
          (AWS_IO_EVENT_TYPE_READABLE | AWS_IO_EVENT_TYPE_WRITABLE),
          on_event,
          /*user_data*/NULL
        );
      default:
        break;
    }
  }
  if (subscribed) {
    aws_task_init(&client_args->task, unsub_task, args, "cleanup");
    aws_event_loop_schedule_task_now(event_loop, &client_args->task);
  }
}

int main() {
  struct aws_allocator *allocator = aws_default_allocator();
  assert(allocator);
  struct aws_event_loop *event_loop = aws_event_loop_new_default(allocator, aws_high_res_clock_get_ticks);
  assert(event_loop);
  ASSERT_SUCCESS(aws_event_loop_run(event_loop));

  struct client_args client_args[NUM_THREADS];
  for (int i=0; i<NUM_THREADS; ++i) {
    client_args[i].event_loop = event_loop;
#ifdef VERIFY
    // need some mechanism for read/writing to files to trigger event handling
    // easiest mechanism is probably to stub the epoll handling
#else
    FILE *file = tmpfile();
    assert(file);
    client_args[i].handle = (struct aws_io_handle){.data.fd = fileno(file), .additional_data = NULL};
#endif

#ifdef VERIFY
    // client_args choices is non-det
#else
    for (int j=0; j<NUM_API_CALL_ROUNDS; ++j) {
      uint8_t val;
      ASSERT_SUCCESS(aws_device_random_u8(&val));
      val /= (UINT8_MAX / (NUM_API_CALLS + 1) + 1);
      client_args[i].choices[j] = val;
    }
#endif
  }
  struct aws_thread tids[NUM_THREADS];
  for (int i=0; i<NUM_THREADS; ++i) {
    ASSERT_SUCCESS(aws_thread_init(&tids[i], allocator));
  }
  for (int i=0; i<NUM_THREADS; ++i) {
    ASSERT_SUCCESS(aws_thread_launch(&tids[i], client, &client_args[i], NULL));
  }
  for (int i=0; i<NUM_THREADS; ++i) {
    ASSERT_SUCCESS(aws_thread_join(&tids[i]));
  }
  aws_event_loop_destroy(event_loop);
  return 0;
}
