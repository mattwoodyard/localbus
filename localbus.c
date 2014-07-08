#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <event.h>

typedef enum { STARTED, ACTIVE, FAILED, STOPPING, RESTARTING} client_status ;

#define CYCLE_MARKER "1|CycleDone\n"

typedef struct _bus_client {
  int recv_fd;
  int send_fd;	
  pid_t procpid;	
  char *proc_name;	
  client_status  cstate;
  struct bufferevent *bev;
  // XXX - should add a message queue
  struct _bus_client **all_clients;
  char cycle_notify;
} bus_client;




void bufread(struct bufferevent *bev, void *data) {
  char *line;
  bus_client **cur;
  bus_client *this_client = (bus_client *) data;
  while ( (line = evbuffer_readline(bev->input)) != NULL ) {
    cur  =  this_client->all_clients;
    do { 
      if (*cur != this_client) {
        //write it
        write((*cur)->send_fd, line, strlen(line));
        write((*cur)->send_fd, "\n", 1);
      }
      cur++;
    } while ( *cur != NULL );
    free(line);
  }

  // XXX - refactor
  cur  =  this_client->all_clients;
  do { 
    if ((*cur)->cycle_notify) {
      //write it
      write((*cur)->send_fd, CYCLE_MARKER , strlen(CYCLE_MARKER));
    }
    cur++;
  } while ( *cur != NULL );



}

int launch_child(char *exec_string, bus_client *client, bus_client **first, char use_cycles) {
  int rc; 
  int childrfd[2];
  int childwfd[2];
  int pid;

  pipe(childrfd);
  pipe(childwfd);

  printf("FD: %d %d\n", childrfd[0], childrfd[1]);

  pid = fork();
  if (pid == 0) {
    dup2(childrfd[0], 0);
    dup2(childwfd[1], 1);
    rc = execlp(exec_string, exec_string, NULL);
    return rc;
  //  exit(-1);
  } else if (pid < 0 ) {
    return pid;
  } else {
    client->all_clients = first;
    client->procpid = pid;
    client->proc_name =exec_string;
    client->cstate = STARTED;
    client->recv_fd = childwfd[0];
    client->send_fd = childrfd[1];
    client->bev = bufferevent_new(client->recv_fd, bufread, NULL, NULL, (void *)client);
    client->cycle_notify = use_cycles;
    bufferevent_enable(client->bev, EV_READ);
  }
  return 0;
}


void usage() {
    fprintf(stderr, "usage:  localbus [-c] command1 [command ...]\n");
    fprintf(stderr, "Bidirectional 'tee'. Stdout from command1 to Stdin of all other commands and vice versa\n");
    fprintf(stderr, "  -c    The bus should operate in cycles, the bus will send a message when the cycle is done. Only applies to commands following -c\n");
    exit(1);

}



int main(int argc, char **argv) {
  int child_count = 0;
  int i,rc ;
  char use_cycles;

  event_init();

  bus_client **clients = (bus_client **)calloc(argc, sizeof(bus_client *));

  for (i = 0 ; i < argc -1; i++ ) {
    *(clients + i ) = malloc(sizeof(bus_client)); 
  }

  *(clients + argc) = NULL;

  //*(clients + argc) = (bus_client *)NULL;

  if (argc == 1) {
    usage();
  }


  // XXX - refactor this
  for (i = 1; i < argc; i++) {
    if (*(argv +i)[0] == '-') {
      switch (*(argv + i) [1]) {
        case 'c':
          use_cycles = 1;
          break;
        default:
          break;
      }
    } else {
      if ((rc = launch_child(*(argv + i), *(clients + child_count), clients, use_cycles)) < 0 ) {
      }
      child_count++;
    }
  }

  event_dispatch();

  return 0;
}
