#include "graphite.h"

int connect_to_graphite_server(u_int32_t ipaddress, int port) {
  return -1;
}
int send_graphite_data() {
  return -1;
}
void graphite_loop_run(struct callmaster* cm, int seconds) {}
void set_prefix(char* prefix) {}
void graphite_loop(void *d) {}
void set_latest_graphite_interval_start(struct timeval *tv) {}
void set_graphite_interval_tv(struct timeval *tv) {}
