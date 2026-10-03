/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

/**
 * @file
 *
 * Feeds each file named on the command line once through
 * LLVMFuzzerTestOneInput(), the entry point a fuzz harness defines. This is
 * `make fuzz-replay`: the corpus and the seeds as a regression test, in an
 * ordinary build, with no libFuzzer and no sanitizer. A crash is the failure.
 */

#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

int main(int argc, char * argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: replay FILE...\n");
    return 2;
  }
  size_t replayed = 0;
  for (int i = 1; i < argc; i++) {
    FILE * file = fopen(argv[i], "rb");
    if (!file) {
      fprintf(stderr, "replay: cannot open %s\n", argv[i]);
      return 2;
    }
    size_t capacity = 4096;
    size_t length = 0;
    uint8_t * buffer = malloc(capacity);
    while (buffer) {
      if (length == capacity) {
        capacity *= 2;
        uint8_t * grown = realloc(buffer, capacity);
        if (!grown) {
          free(buffer);
          buffer = NULL;
          break;
        }
        buffer = grown;
      }
      size_t got = fread(buffer + length, 1, capacity - length, file);
      if (got == 0) {
        break;
      }
      length += got;
    }
    fclose(file);
    if (!buffer) {
      fprintf(stderr, "replay: out of memory reading %s\n", argv[i]);
      return 2;
    }
    LLVMFuzzerTestOneInput(buffer, length);
    free(buffer);
    replayed++;
  }
  printf("replay: %zu inputs, no crash\n", replayed);
  return 0;
}
