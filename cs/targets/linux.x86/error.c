#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

uint32_t blinker_pattern;

void resetblink(uint32_t pattern) {
  blinker_pattern = pattern;
  printf("blink %x\n", pattern);
}

void diewith(uint32_t pattern) {
  fprintf(stderr, "Diewith: %0x\n", pattern);
  abort();
}
