#include "datacenter.h"
#include "datacenter_utils.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static void *reaper_thread_main(void *arg) {
  DataCenter *dc = (DataCenter *)arg;

  while (1) {
    pthread_mutex_lock(&dc->reaper_lock);
    while (dc->active_vms == 0 && !dc->shutting_down) {
      pthread_cond_wait(&dc->reaper_cond, &dc->reaper_lock);
    }
    if (dc->active_vms == 0 && dc->shutting_down) {
      pthread_mutex_unlock(&dc->reaper_lock);
      break;
    }
    pthread_mutex_unlock(&dc->reaper_lock);

    int status;
    pid_t pid = waitpid(-1, &status, 0);
    if (pid > 0) {
      reap_vm_by_pid(dc, pid);
    } else if (pid < 0 && errno == ECHILD) {
      pthread_mutex_lock(&dc->reaper_lock);
      dc->active_vms = 0;
      pthread_cond_broadcast(&dc->reaper_cond);
      pthread_mutex_unlock(&dc->reaper_lock);
    }
  }

  return NULL;
}

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;
  dc->host_cores = 1;

  dc->num_vm_types = 0;

  dc->num_reservations = 0;

  pthread_rwlock_init(&dc->vm_types_lock, NULL);
  pthread_mutex_init(&dc->reservations_lock, NULL);
  pthread_mutex_init(&dc->reaper_lock, NULL);
  pthread_cond_init(&dc->reaper_cond, NULL);

  dc->active_vms = 0;
  dc->shutting_down = 0;
  dc->reaper_started = 0;

  dc->configured = 0;
}

void datacenter_set_host_cores(DataCenter *dc, size_t host_cores) {
  dc->host_cores = (host_cores > 0) ? host_cores : 1;
}

void datacenter_destroy(DataCenter *dc) {
  if (dc->reaper_started) {
    pthread_mutex_lock(&dc->reaper_lock);
    dc->shutting_down = 1;
    pthread_cond_broadcast(&dc->reaper_cond);
    pthread_mutex_unlock(&dc->reaper_lock);

    pthread_join(dc->reaper_thread, NULL);
    dc->reaper_started = 0;
  }

  for (size_t i = 0; i < dc->num_servers; i++) {
    for (size_t j = 0; j < dc->servers[i].num_hosted_vms; j++) {
      free(dc->servers[i].hosted_vms[j]);
    }
    pthread_mutex_destroy(&dc->servers[i].lock);
  }
  free(dc->servers);
  dc->servers = NULL;

  pthread_rwlock_destroy(&dc->vm_types_lock);
  pthread_mutex_destroy(&dc->reservations_lock);
  pthread_mutex_destroy(&dc->reaper_lock);
  pthread_cond_destroy(&dc->reaper_cond);
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    pthread_mutex_init(&dc->servers[i].lock, NULL);
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  if (pthread_create(&dc->reaper_thread, NULL, reaper_thread_main, dc) == 0) {
    dc->reaper_started = 1;
  } else {
    fprintf(stderr, "Failed to start reaper thread.\n");
    return 1;
  }

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  VMType resolved_type = *type;
  if (absolute_path(type->exec_path, resolved_type.exec_path, sizeof(resolved_type.exec_path)) != 0) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (absolute_path(type->input_folder, resolved_type.input_folder, sizeof(resolved_type.input_folder)) != 0) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  pthread_rwlock_wrlock(&dc->vm_types_lock);

  if (VMType_exists(dc, resolved_type.id) != NULL) {
    pthread_rwlock_unlock(&dc->vm_types_lock);
    fprintf(stderr, "VM type with id %s already exists.\n", resolved_type.id);
    return 1;
  }

  if (dc->num_vm_types >= MAX_VM_TYPES) {
    pthread_rwlock_unlock(&dc->vm_types_lock);
    fprintf(stderr, "Maximum of VM types reached.\n");
    return 1;
  }

  dc->vm_types[dc->num_vm_types++] = resolved_type;

  pthread_rwlock_unlock(&dc->vm_types_lock);

  return 0;
}

static size_t collect_sorted_servers(const Reservation *reservation, size_t *out_server_ids) {
  size_t count = 0;

  for (size_t i = 0; i < reservation->items_count; i++) {
    size_t sid = reservation->items[i].server_id;
    int exists = 0;
    for (size_t j = 0; j < count; j++) {
      if (out_server_ids[j] == sid) {
        exists = 1;
        break;
      }
    }
    if (!exists) {
      out_server_ids[count++] = sid;
    }
  }

  for (size_t i = 0; i < count; i++) {
    for (size_t j = i + 1; j < count; j++) {
      if (out_server_ids[j] < out_server_ids[i]) {
        size_t tmp = out_server_ids[i];
        out_server_ids[i] = out_server_ids[j];
        out_server_ids[j] = tmp;
      }
    }
  }

  return count;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  reap_finished_vms_nonblocking(dc);

  pthread_rwlock_rdlock(&dc->vm_types_lock);

  size_t total_requested_vms = 0;
  for (size_t i = 0; i < reservation->items_count; i++) {
    ReservationItem *item = &reservation->items[i];

    if (item->amount == 0) {
      pthread_rwlock_unlock(&dc->vm_types_lock);
      fprintf(stderr, "Reservation item must request at least one VM.\n");
      return 1;
    }

    VMType *vm_type = VMType_exists(dc, item->vm_type_id);
    if (vm_type == NULL) {
      pthread_rwlock_unlock(&dc->vm_types_lock);
      fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
      return 1;
    }

    if (item->server_id == 0 || item->server_id > dc->num_servers) {
      pthread_rwlock_unlock(&dc->vm_types_lock);
      fprintf(stderr, "Invalid server in reservation.\n");
      return 1;
    }

    total_requested_vms += item->amount;
    if (total_requested_vms > MAX_RESERVATION_VMS) {
      pthread_rwlock_unlock(&dc->vm_types_lock);
      fprintf(stderr, "Reached max of VMS for a reservation.\n");
      return 1;
    }

    item->vm_type = vm_type;
  }

  pthread_rwlock_unlock(&dc->vm_types_lock);

  size_t locked_servers[MAX_RESERVATIONS_ITEMS];
  size_t num_locked = collect_sorted_servers(reservation, locked_servers);

  for (size_t i = 0; i < num_locked; i++) {
    pthread_mutex_lock(&dc->servers[locked_servers[i] - 1].lock);
  }

  Resources sim_available[MAX_RESERVATIONS_ITEMS];
  size_t sim_hosted[MAX_RESERVATIONS_ITEMS];
  for (size_t i = 0; i < num_locked; i++) {
    Server *srv = &dc->servers[locked_servers[i] - 1];
    sim_available[i] = srv->available;
    sim_hosted[i] = srv->num_hosted_vms;
  }

  for (size_t i = 0; i < reservation->items_count; i++) {
    ReservationItem *item = &reservation->items[i];
    size_t srv_idx = 0;
    for (size_t k = 0; k < num_locked; k++) {
      if (locked_servers[k] == item->server_id) {
        srv_idx = k;
        break;
      }
    }

    for (size_t j = 0; j < item->amount; j++) {
      if (sim_hosted[srv_idx] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        for (size_t u = num_locked; u > 0; u--) {
          pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
        }
        return 1;
      }

      if (!resources_can_fit(sim_available[srv_idx], item->vm_type->required)) {
        fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
        for (size_t u = num_locked; u > 0; u--) {
          pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
        }
        return 1;
      }

      resources_sub(&sim_available[srv_idx], item->vm_type->required);
      sim_hosted[srv_idx]++;
    }
  }

  pthread_mutex_lock(&dc->reservations_lock);

  if (reservation_exists(dc, reservation->id)) {
    fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
    pthread_mutex_unlock(&dc->reservations_lock);
    for (size_t u = num_locked; u > 0; u--) {
      pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
    }
    return 1;
  }

  if (dc->num_reservations >= MAX_RESERVATIONS) {
    fprintf(stderr, "Max number of reservations reached\n");
    pthread_mutex_unlock(&dc->reservations_lock);
    for (size_t u = num_locked; u > 0; u--) {
      pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
    }
    return 1;
  }

  if (reservation_commit(dc, reservation) != 0) {
    pthread_mutex_unlock(&dc->reservations_lock);
    for (size_t u = num_locked; u > 0; u--) {
      pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
    }
    return 1;
  }

  size_t num_vms = reservation->num_vms;
  char vm_dirs[MAX_RESERVATION_VMS][MAX_PATH_SIZE];
  char input_dirs[MAX_RESERVATION_VMS][MAX_PATH_SIZE];
  for (size_t i = 0; i < num_vms; i++) {
    memcpy(vm_dirs[i], reservation->vms[i]->dir_path, MAX_PATH_SIZE);
    memcpy(input_dirs[i], reservation->vms[i]->type->input_folder, MAX_PATH_SIZE);
  }

  dc->reservations[dc->num_reservations] = *reservation;
  dc->num_reservations++;

  pthread_mutex_unlock(&dc->reservations_lock);

  for (size_t u = num_locked; u > 0; u--) {
    pthread_mutex_unlock(&dc->servers[locked_servers[u - 1] - 1].lock);
  }

  for (size_t i = 0; i < num_vms; i++) {
    if (copy_directory_contents(input_dirs[i], vm_dirs[i]) != 0) {
      fprintf(stderr, "Failed to prepare VM directory %s.\n", vm_dirs[i]);
      return 1;
    }
  }

  return 0;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  pthread_mutex_lock(&dc->reservations_lock);

  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) {
    pthread_mutex_unlock(&dc->reservations_lock);
    return 1;
  }

  for (size_t i = 0; i < res->num_vms; i++) {
    VM *vm = res->vms[i];
    double denom = vm->server->total.cpu * (double)dc->num_servers;
    double raw_pct = (denom > 0.0)
        ? (100.0 * vm->type->required.cpu * (double)dc->host_cores) / denom
        : 100.0;
    int max_limit = (int)(100 * dc->host_cores);
    int cpu_limit = (int)round(raw_pct);
    if (cpu_limit < 1) {
      cpu_limit = 1;
    } else if (cpu_limit > max_limit) {
      cpu_limit = max_limit;
    }
    vm->cpu_limit = cpu_limit;
  }

  pthread_mutex_lock(&dc->reaper_lock);
  dc->active_vms += res->num_vms;
  pthread_mutex_unlock(&dc->reaper_lock);

  if (spawn_all_vms(res) != 0) {
    size_t spawned = 0;
    for (size_t i = 0; i < res->num_vms; i++) {
      if (res->vms[i]->state == VM_STATE_RUNNING) {
        spawned++;
      }
    }
    pthread_mutex_lock(&dc->reaper_lock);
    if (dc->active_vms >= (res->num_vms - spawned)) {
      dc->active_vms -= (res->num_vms - spawned);
    } else {
      dc->active_vms = 0;
    }
    pthread_cond_broadcast(&dc->reaper_cond);
    pthread_mutex_unlock(&dc->reaper_lock);

    pthread_mutex_unlock(&dc->reservations_lock);
    return 1;
  }

  res->state = RES_STATE_RUNNING;

  pthread_mutex_unlock(&dc->reservations_lock);

  pthread_mutex_lock(&dc->reaper_lock);
  pthread_cond_broadcast(&dc->reaper_cond);
  pthread_mutex_unlock(&dc->reaper_lock);

  return 0;
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  reap_finished_vms_nonblocking(dc);

  pthread_rwlock_rdlock(&dc->vm_types_lock);
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_lock(&dc->servers[i].lock);
  }
  pthread_mutex_lock(&dc->reservations_lock);
  flockfile(stdout);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server server = dc->servers[i];

    Resources used = server.total;
    resources_sub(&used, server.available);

    printf(" Server #%zu\n", server.id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.total.cpu, server.total.ram, server.total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.available.cpu, server.available.ram, server.available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server.num_hosted_vms);

    if (server.num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server.num_hosted_vms; j++) {
        VM *vm = server.hosted_vms[j];
        const char *prefix = (j == server.num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");
  fflush(stdout);
  funlockfile(stdout);

  pthread_mutex_unlock(&dc->reservations_lock);
  for (size_t i = dc->num_servers; i > 0; i--) {
    pthread_mutex_unlock(&dc->servers[i - 1].lock);
  }
  pthread_rwlock_unlock(&dc->vm_types_lock);

  return 0;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}