/*
 *  Copyright (c) 2012, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "bloom.h"

#ifdef __linux
#include <sys/time.h>
#include <time.h>
#endif


/** ***************************************************************************
 * A few simple tests to check if it works at all.
 *
 */
static void basic()
{
  (void)printf("----- basic -----\n");

  struct bloom bloom;

  assert(bloom_init(&bloom, 0, 1.0) == 1);
  assert(bloom_init(&bloom, 10, 0) == 1);
  assert(bloom.ready == 0);
  assert(bloom_add(&bloom, "hello world", 11) == -1);
  assert(bloom_check(&bloom, "hello world", 11) == -1);
  bloom_free(&bloom);

  assert(bloom_init(&bloom, 102, 0.1) == 0);
  assert(bloom.ready == 1);
  bloom_print(&bloom);

  assert(bloom_check(&bloom, "hello world", 11) == 0);
  assert(bloom_add(&bloom, "hello world", 11) == 0);
  assert(bloom_check(&bloom, "hello world", 11) == 1);
  assert(bloom_add(&bloom, "hello world", 11) > 0);
  assert(bloom_add(&bloom, "hello", 5) == 0);
  assert(bloom_add(&bloom, "hello", 5) > 0);
  assert(bloom_check(&bloom, "hello", 5) == 1);
  bloom_free(&bloom);
}


/** ***************************************************************************
 * Create a bloom filter with given parameters and add 'count' random elements
 * into it to see if collission rates are within expectations.
 *
 */
static void add_random(int entries, double error, int count)
{
  (void)printf("----- add_random(%d, %f, %d) -----\n", entries, error, count);

  struct bloom bloom;
  assert(bloom_init(&bloom, entries, error) == 0);
  bloom_print(&bloom);

  char block[32];
  int collisions = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  int n;

  assert(fd >= 0);
  for (n = 0; n < count; n++) {
    assert(read(fd, block, 32) == 32);
    if (bloom_add(&bloom, (void *)block, 32)) { collisions++; }
  }
  (void)close(fd);
  bloom_free(&bloom);

  (void)printf("added %d elements, got %d collisions\n", count, collisions);

  if (count <= entries) {
    assert(collisions <= (entries * error));
  } else if (count <= entries * 2) {
    assert(collisions < (2 * entries * error));
  }
}


/** ***************************************************************************
 * Simple loop to compare performance.
 *
 */
static void perf_loop(int entries, int count)
{
  (void)printf("----- perf_loop -----\n");

  struct bloom bloom;
  assert(bloom_init(&bloom, entries, 0.001) == 0);
  bloom_print(&bloom);

  int i;
  int collisions = 0;

  struct timeval tp;
  (void)gettimeofday(&tp, NULL);
  long before = (tp.tv_sec * 1000L) + (tp.tv_usec / 1000L);

  for (i = 0; i < count; i++) {
    if (bloom_add(&bloom, (void *)&i, sizeof(int))) { collisions++; }
  }

  (void)gettimeofday(&tp, NULL);
  long after = (tp.tv_sec * 1000L) + (tp.tv_usec / 1000L);

  (void)printf("Added %d elements of size %d, took %d ms (collisions=%d)\n",
               count, (int)sizeof(int), (int)(after - before), collisions);

  (void)printf("%d,%d,%ld\n", entries, bloom.bytes, after - before);
  bloom_free(&bloom);
}


/** ***************************************************************************
 * main...
 *
 */
int main(int argc, char **argv)
{
  (void)printf("testing libbloom...\n");

  if (argc == 4 && !strncmp(argv[1], "-p", 2)) {
    perf_loop(atoi(argv[2]), atoi(argv[3]));
    exit(0);
  }

  basic();
  add_random(100, 0.001, 300);

  int i;
  for (i = 0; i < 10; i++) {
    add_random(1000000, 0.001, 1000000);
  }

  perf_loop(10000000, 10000000);
}
