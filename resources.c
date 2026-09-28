#include "resources.h"

#include <math.h>

#define CPU_EPSILON 1e-9


int resources_cpu_is_integer(double cpu){
  return fabs(cpu - round(cpu)) < CPU_EPSILON;
}

int resources_can_fit(Resources available, Resources required) {
  return available.ram >= required.ram &&
         available.disk >= required.disk &&
         available.cpu >= required.cpu;
}

void resources_add(Resources *dst, Resources src) {
  dst->ram += src.ram;
  dst->disk += src.disk;
  dst->cpu += src.cpu;
}

void resources_sub(Resources *dst, Resources src) {
  dst->ram -= src.ram;
  dst->disk -= src.disk;
  dst->cpu -= src.cpu;
}

int resources_equal(Resources a, Resources b) {
    return a.ram == b.ram &&
           a.disk == b.disk &&
           fabs(a.cpu - b.cpu) < CPU_EPSILON;
}