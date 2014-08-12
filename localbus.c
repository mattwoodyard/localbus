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
#include <stdio.h>
#include <stdlib.h> 
#include <signal.h>
#include <sys/wait.h>

typedef enum { STARTED, ACTIVE, FAILED, STOPPING, RESTARTING} client_status ;
typedef enum { MSG_TICK, MSG_ACK, MSG_MSG, MSG_SUB_ADD, MSG_SUB_REM} msg_type ;


static const char *msg_type_str[] = {"TICK", "ACK", "MSG", "ADD", "REM"};  

#define CYCLE_MARKER "1|CycleDone\n"
#define TICK_MARKER "1|Tick\n"
#define ACK "1|Ack\n"


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
  char message_acked;
  int counter;
  int idx;

} bus_client;


typedef struct _msg {
  bus_client *sender;
  char *message_data;
  msg_type mtype;
  unsigned int acks; 
  TAILQ_ENTRY(_msg) msgs;
} message;


typedef struct _msg_queue {
  TAILQ_HEAD(msg_list, _msg) messages;
  struct event *mq_event;
  message *active_message;
  
  char tick_active;
  char message_sent;
  char first_message;
} msg_queue;


typedef struct _cb_data {
  bus_client *bc;
  msg_queue *mq;
}callback_struct;

typedef struct _sig_cb_data {
  bus_client **bc;
  msg_queue *mq;
}signal_callback_struct;






// TAILQ_INIT(&head); 

message * message_create(bus_client *sender, char * message_data, msg_type ctype) {
  message *m = (message *) calloc(1, sizeof(message));
  m->sender = sender;
  m->message_data = message_data;
  m->mtype = ctype;
  return m;
}

void message_free(message *m) {
  if (m->message_data) {
    free(m->message_data);
  }
  free(m);
}

void message_push(msg_queue *mq, message *m) {
  TAILQ_INSERT_TAIL(&mq->messages, m, msgs);
  // event_active(mq->mq_event, EV_READ, 0);
}

char message_is_ack( message *m) {
  return m->mtype == MSG_ACK;
}

void message_show(message *m) {
  printf("%s -> %s [%s]\n", m->sender->proc_name, msg_type_str[m->mtype], m->message_data); 
}

#define tickAck(c) c->tick_acked = 1; \
                   c->counter++                
#define tick(c) c->tick_acked = 0 


char is_ack(const char *line) {
  return line[0]=='1' && line[1] == '|' && \
                 line[2] == 'A' && line[3] == 'c' && line[4] == 'k';
}

char is_first_tick(bus_client *this_client) {
  return this_client->counter == 0;
}

void broadcast(bus_client *this_client, bus_client **clients, const char * message) {
  bus_client **cur = clients;
  do { 
    if (*cur != this_client && (*cur)->cstate == ACTIVE) {
      write((*cur)->send_fd, message, strlen(message));
      write((*cur)->send_fd, "\n", 1);
    }
    cur++;
  } while ( *cur != NULL );
}

void mq_flush(msg_queue *mq , bus_client **clients) { 
  message *cmsg;
  while (mq->messages.tqh_first != NULL) { 
    cmsg = mq->messages.tqh_first;
    broadcast(NULL, clients, cmsg->message_data);
    TAILQ_REMOVE(&mq->messages, mq->messages.tqh_first, msgs);
    message_free(cmsg);
  }
} 

int message_count(msg_queue *mq) {
  int i = 0;
  message *cmsg;
  TAILQ_FOREACH(cmsg, &mq->messages, msgs) {
    i++;
  }
  return i;
}

message * message_pop(msg_queue *mq) {
  message *cmsg;
  if (mq->messages.tqh_first !=NULL) {
    cmsg = mq->messages.tqh_first;
    // printf("POP: %s\n", cmsg->message_data);
    TAILQ_REMOVE(&mq->messages, mq->messages.tqh_first, msgs);
    return cmsg;  
  }

  return NULL;
}


void next_message(bus_client **clients) {
  bus_client **head = clients;
  while (*head != NULL) { 
   (*head)->message_acked = 0; 
    head++;
  } 
  


}


/*
 * The problem is that we don't require ticks to be acked...
 * We need ticks to be acked 
 * The ticks not being acked are a problem in that we moving forward
 *  - push a phantom message into "active tick"
 *
 */
void send_tick(bus_client **clients) {
  broadcast(NULL, clients, "1|Tick");
  next_message(clients);
}

void dump_state(bus_client **clients, msg_queue *mq) {
  int i=0;

  printf("Q(%d|", message_count(mq));
  printf("%s|", mq->message_sent != 0 ? " SENT " : "UNSENT" );
  printf("%s|", mq->active_message != NULL ? " MSG " : "NOMSG" );
  printf("%s):[", mq->tick_active != 0 ? " TICK " : "NOTICK" );
  bus_client **head = clients;
  while (*head != NULL) { 
    if ((*head)->cstate == ACTIVE) {
      printf("%d=>%d ",(*head)->procpid, (*head)->message_acked); //(*head)->cstate == ACTIVE ? "A" : "I" );
    }
    head++;
    i++;
  } 
  printf("]\n");

}


char message_finalized(bus_client **clients) {

  bus_client **head = clients;
  while (*head != NULL) { 

    if ((*head)->cstate == ACTIVE && (*head)->message_acked ==0 ) { return 0; }
    head++;
  } 
  return 1;
}



void handle_ack(callback_struct *cbs, bus_client *this_client) {
  
  if (cbs->mq->active_message) { this_client->message_acked = 1; }
  else if (cbs->mq->tick_active) { this_client->message_acked = 1; }
  else { printf("Error Ack for nothing\n");}

  if (message_finalized(this_client->all_clients)) {
    if (cbs->mq->active_message) { 
      free(cbs->mq->active_message);
      cbs->mq->active_message = NULL;
    }
    next_message(this_client->all_clients);
    cbs->mq->message_sent = 0;
    cbs->mq->tick_active = 0;
  }
}




void bufread(struct bufferevent *bev, void *data) {
  char *line;
  callback_struct *cbs = (callback_struct *)data;
  bus_client *this_client = cbs->bc;
  msg_type cur_type = MSG_TICK;
   


  /* if we are an ack
   *    then we mark the clinent as acing hte current message
   *    otherwise we push
   */


  // printf("-----------------------\n");
  while ((line = evbuffer_readline(bev->input)) != NULL ) {
  //  printf("RECV[%s:%d]: %s\n",this_client->proc_name, this_client->procpid, line);
  //  dump_state(this_client->all_clients, cbs->mq);
    if (is_ack(line)) { cur_type = MSG_ACK; } else { cur_type = MSG_MSG; } 

    if (cur_type != MSG_ACK) {
      message_push(cbs->mq, message_create(this_client, line, cur_type));
    } else { /* We received an ack */
      handle_ack(cbs, this_client);
    }
  }

//  dump_state(this_client->all_clients, cbs->mq);

  if (cbs->mq->active_message == NULL && cbs->mq->tick_active == 0) {
      free(cbs->mq->active_message);
      cbs->mq->active_message = message_pop(cbs->mq);
      if (cbs->mq->active_message == NULL) {
        cbs->mq->tick_active  = 1;   
      }
  }

  if (!cbs->mq->message_sent) {
    if(cbs->mq->active_message) { broadcast(NULL, this_client->all_clients, cbs->mq->active_message->message_data); }
    else if (cbs->mq->tick_active) { send_tick(this_client->all_clients); }
    cbs->mq->message_sent =1;
  }


  if(cbs->mq->active_message) {
    if(!cbs->mq->message_sent) {
     // printf("SEND: %s\n",cbs->mq->active_message->message_data);
      broadcast(NULL, this_client->all_clients, cbs->mq->active_message->message_data);
      cbs->mq->message_sent = 1;
    } 
  } else {
    if(!cbs->mq->message_sent) { 
      send_tick(this_client->all_clients);
      cbs->mq->tick_active = 1;
      cbs->mq->message_sent =1;
    }
  }
  // dump_state(this_client->all_clients, cbs->mq);
}





int launch_child(char *exec_string, bus_client *client, bus_client **first, msg_queue *mq, char use_cycles, int idx) {
  int rc; 
  int childrfd[2];
  int childwfd[2];
  int pid;
  // int flags; 

  pipe(childrfd);
  pipe(childwfd);

  pid = fork();
  if (pid == 0) {
    dup2(childrfd[0], 0);
    dup2(childwfd[1], 1);


    // flags = fcntl(0, F_GETFL);
    // fcntl(0, F_SETFL, flags| O_NONBLOCK);
    // flags = fcntl(1, F_GETFL);
    // fcntl(1, F_SETFL, flags| O_NONBLOCK);


    rc = execlp(exec_string, exec_string, NULL);
    fprintf(stderr,  "Error exec %s %d\n",exec_string, rc);
    exit(-1);
  } else if (pid < 0 ) {
    return pid;
  } else {
    callback_struct *cbs = (callback_struct *)calloc(1, sizeof(callback_struct));  
    cbs->mq = mq;
    cbs->bc = client;

    client->all_clients = first;
    client->procpid = pid;
    client->proc_name =exec_string;
    client->cstate = ACTIVE;
    client->recv_fd = childwfd[0];
    client->send_fd = childrfd[1];
    client->bev = bufferevent_new(client->recv_fd, bufread, NULL, NULL, (void *)cbs);
    client->cycle_notify = use_cycles;
    client->message_acked = 0;
    client->counter = 0;
    client->idx = idx;
    bufferevent_enable(client->bev, EV_READ);
    //TAILQ_INIT(&client->messages);
  }
  return 0;
}


void usage() {
    fprintf(stderr, "usage:  localbus [-c] command1 [command ...]\n");
    fprintf(stderr, "Bidirectional 'tee'. Stdout from command1 to Stdin of all other commands and vice versa\n");
  //  fprintf(stderr, "  -c    The bus should operate in cycles, the bus will send a message when the cycle is done. Only applies to commands following -c\n");
    exit(1);

}

void signal_cb(int fd, short event, void *data) {
  signal_callback_struct *cbs = (signal_callback_struct *)data;
  bus_client **clients = cbs->bc;
  bus_client **cur = clients;
  msg_queue *mq = cbs->mq ;
  int rc, status;
  char active = 0;


  rc = waitpid(-1, &status, WNOHANG);
  

  do {
    // printf("Child: %d done\n", rc);
    cur = clients;
    do {
      if (rc == (*cur)->procpid) { 
        printf("SHUTDOWN %s %d\n", (*cur)->proc_name, (*cur)->procpid);
        (*cur)->cstate = STOPPING;
      } 
      cur++;
    } while ( *cur != NULL );
    rc = waitpid(-1, &status, WNOHANG);
  } while (rc > 0);


  cur = clients;
  do {
    if ((*cur)->cstate == ACTIVE ) { active = 1; }
    cur++;
  } while ( *cur != NULL );
    
    mq_flush(mq, clients);
    send_tick(clients);


  if (!active) {
    //       What do we need to free up?
    //        a bunch of libevent stuff 
    //        shutdown the eventloop
    //        clean up any events
    //        free the clients array
    // XXX - shutdown clean up
    //       its not really necessary to free things, but not freeing is lazy, anyway do that at somepoint
    exit(0);
  }

}






int main(int argc, char **argv) {
  int child_count = 0;
  int i,rc ;
  char use_cycles;
  struct event_base *base;
  struct event *signal_int;
  msg_queue mq;

  TAILQ_INIT(&mq.messages);

  base =event_init();

  bus_client **clients = (bus_client **)calloc(argc, sizeof(bus_client *));

  // mq.mq_event = event_new(base, -1, 0, , (void *) &mq);
  mq.active_message = NULL;
  mq.message_sent = 0;
  mq.first_message = 1;
  mq.tick_active = 1;


  for (i = 0 ; i < argc -1; i++ ) {
    *(clients + i ) = malloc(sizeof(bus_client)); 
  }

  *(clients + argc) = NULL;

  //*(clients + argc) = (bus_client *)NULL;

  if (argc == 1) {
    usage();
  }

  // XXX - always cycle for now
  //       the cycle flag introduces a bug 
  //       we should check to see that there is use_cycle in the can tick function
  // XXX - refactor this
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      switch (argv[i][1]) {
        case 'c':
          use_cycles = 1;
          break;
        default:
          break;
      }
    } else {
      printf("To Launch: %s:%d\n",argv[i], use_cycles);
      if ((rc = launch_child(*(argv + i), *(clients + child_count), clients, &mq, 1, i - 1 )) < 0 ) {
      }
      child_count++;
    }
  }
  
  signal_callback_struct cbs = { clients, &mq };
  
  signal_int = evsignal_new(base, SIGCHLD, signal_cb, (void *) &cbs);
  event_add(signal_int, NULL);

  event_dispatch();

  return 0;
}
