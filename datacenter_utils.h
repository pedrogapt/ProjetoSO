#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include "datacenter.h"

/**
 * Checks if a VMType with the given id exists.
 * 
 * @param dc Pointer to Data Center.
 * @param type_id Id of VM type being checked.
 * 
 * @return Pointer to VMType if it exists.
 * @return NULL if VMType does not exist.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Simulates the placement of every VM requested in the reservation using a
 * temporary copy of each server's available resources. The Data Center state
 * is left unchanged regardless of the outcome.
 *
 * Validation fails if any requested server does not have enough available
 * resources to host all the requested VMs assigned to it or if the request 
 * does not have valid parameters (VM types that do not exist, invalid server ids).
 *
 * @param dc Data Center where the reservation will be validated.
 * @param reservation Reservation to validate.
 *
 * @return 0 if the reservation can be fully allocated.
 * @return 1 otherwise.
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

/**
 * Creates all VMs requested by the reservation, assigns them to their
 * designated servers, updates the server resource availability, and stores
 * references to the created VMs in both the reservation and the corresponding
 * server hosted VM list.
 *
 * The reservation is expected to have already passed validation before this
 * function is called. If an error occurs during the commit process, the
 * function performs a rollback of the changes made by this operation.
 *
 * @param dc Data Center where the reservation will be committed.
 * @param reservation Reservation to allocate.
 *
 * @return 0 if the reservation was successfully committed
 * @return 1 if an error occurred and the changes were rolled back.
 */
int reservation_commit(DataCenter *dc, Reservation *reservation);

/**
 * Looks up a reservation by id and checks that it is pending.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to look up.
 *
 * @return Pointer to the Reservation if found and RES_STATE_PENDING.
 * @return NULL if not found or not in RES_STATE_PENDING (error is printed).
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Frees every VM allocated to the reservation, returns their resources to the
 * corresponding hosting servers, removes them from each server's hosted VM
 * list, and finally removes the reservation from the Data Center reservation
 * list.
 *
 * @param dc Pointer to the Data Center containing the reservation.
 * @param reservation Pointer to the reservation to destroy.
 */
void reservation_destroy(DataCenter *dc, Reservation *reservation);

/**
 * Runs in the child process after fork(): sets resource limits (RAM,
 * disk) and execs the VM's workload under cpulimit. Never returns
 * (either execs successfully or calls exit(1) on failure).
 *
 * @param vm Pointer to the VM being spawned.
 */
void spawn_vm_child(VM *vm);

/**
 * Forks one child per VM in the reservation, calling spawn_vm_child()
 * in each child and recording the pid/state in the parent.
 *
 * @param res Pointer to the Reservation whose VMs should be spawned.
 *
 * @return 0 on success.
 * @return 1 if any fork() call fails.
 */
int spawn_all_vms(Reservation *res);

/**
 * Blocks until every VM's process has exited, updating each VM's state
 * to VM_STATE_TERMINATED as its pid is reaped.
 *
 * @param res Pointer to the Reservation whose VMs should be waited on.
 */
void wait_for_all_vms(Reservation *res);

/**
 * Converts a VM state to a human-readable string. Used by datacenter_list().
 *
 * @param state The VMState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *vm_state_to_string(VMState state);

/**
 * Converts a reservation state to a human-readable string. Used by
 * datacenter_list().
 *
 * @param state The ReservationState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H