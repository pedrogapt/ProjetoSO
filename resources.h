#ifndef RESOURCES__H
#define RESOURCES__H

#include <stddef.h>

typedef struct {
    size_t ram;  // RAM in GB
    size_t disk; // Disk Space in GB
    double cpu;  // Number of cores/Vcpu
} Resources;

/**
 * This function verifies if a double precision CPU value is sufficiently
 * close to an integer, accounting for floating-point precision errors.
 *
 * @param cpu CPU value to check.
 *
 * @return 1 if the value is an integer (within the allowed precision),
 * @return 0 otherwise.
 */
int resources_cpu_is_integer(double cpu);

/**
 * Checks whether a resource set can satisfy another.
 *
 * @param available Available resources.
 * @param required Required resources.
 *
 * @return 1 if available resources are sufficient, 0 otherwise.
 */
int resources_can_fit(Resources available, Resources required);

/**
 * Adds one resource set to another.
 *
 * @param dst Resource set to update.
 * @param src Resources to add.
 */
void resources_add(Resources *dst, Resources src);

/**
 * Subtracts one resource set from another.
 *
 * Assumes that dst contains at least the resources in src.
 *
 * @param dst Resource set to update.
 * @param src Resources to subtract.
 */
void resources_sub(Resources *dst, Resources src);

/**
 * Checks whether two resource sets are equal.
 *
 * @param a First resource set.
 * @param b Second resource set.
 *
 * @return 1 if equal, 0 otherwise.
 */
int resources_equal(Resources a, Resources b);

#endif