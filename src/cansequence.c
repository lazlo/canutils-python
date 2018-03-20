//#include <can_config.h>

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include <net/if.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>

#include <linux/can.h>
#include <linux/can/raw.h>

extern int optind, opterr, optopt;

static int s = -1;
static int running = 1;

// statistical data
int rcvdgood = 0,   // frames received in sequence since last error
    rcvdbad  = 0,   // total frames received out of sequence
    frmsent  = 0,   // total frames sent
    frmrcvd  = 0,   // total frames received
    nintr    = 0,   // number of interrupted read/write calls
    nnobufs  = 0;   // number of buffer overflows at write

int greceive = 0;   // cmd option as global var, for showstats()

enum {
  VERSION_OPTION = CHAR_MAX + 1,
};

#define CAN_ID_DEFAULT  (0x22222222)

// long long to byte
#define U64TO8(b, l) (b[0] = (l>>0 )&0xff, \
                      b[1] = (l>>8 )&0xff, \
                      b[2] = (l>>16)&0xff, \
                      b[3] = (l>>24)&0xff, \
                      b[4] = (l>>32)&0xff, \
                      b[5] = (l>>40)&0xff, \
                      b[6] = (l>>48)&0xff, \
                      b[7] = (l>>56)&0xff)

#define U8TO64(l, b) (l =   (b[0]) \
                          + (b[1]<<8)  \
                          + (b[2]<<16) \
                          + (b[3]<<24) \
                          + (((uint64_t)b[4]<<32)) \
                          + (((uint64_t)b[5]<<40)) \
                          + (((uint64_t)b[6]<<48)) \
                          + (((uint64_t)b[7]<<56)) )



void print_usage(char *prg)
{
  fprintf(stderr, "Usage: %s [<can-interface>] [Options]\n"
    "\n"
    "canseq sends CAN messages with a rising seq number as payload.\n"
    "When the -r option is given, canseq expects to receive these messages\n"
    "and prints an error message if a wrong seq number is encountered.\n"
    "The main purpose of this program is to test the reliability of CAN links.\n"
    "\n"
    "Options:\n"
    " -e, --extended      send extended frame\n"
    " -i, --identifier=ID CAN Identifier (default = %u)\n"
    " -r, --receive       work as receiver\n"
    "     --loop=COUNT    send message COUNT times\n"
    " -p, --poll          use poll(2) to wait for buffer space while sending\n"
    " -s, --sleep=US      sleep US microseconds before sending\n"
    " -t, --stat=INTERVAL display statistics every INTERVAL seconds\n"
    " -q, --quit          quit if a wrong seq is encountered\n"
    " -v, --verbose       be verbose (twice to be even more verbose\n"
    " -h, --help          this help\n"
    "     --version       print version information and exit\n",
    prg, CAN_ID_DEFAULT);
}


void showstats(int receive, int statintv)
{
  static time_t lastout;
  static int callcnt = -1;
  static time_t acttm;

  if ( statintv<0 ) return;

  if ( callcnt==-1 ) {
   callcnt = 0;
   time(&lastout);
  }

  time(&acttm);
  if ( acttm-lastout>=statintv ) {
   time(&lastout);
   if ( --callcnt<=0 ) {
     if ( receive ) {
       fprintf(stderr, "%12s %12s %12s\n",
                        "received", "received", "received");
       fprintf(stderr, "%12s %12s %12s\n",
               "total", "last in seq", "total OOS");
     } else {
       fprintf(stderr, "%12s %12s %12s\n", "sent", "", "");
       fprintf(stderr, "%12s %12s %12s\n",
               "total", "EINTR", "ENOBUFS");
     }
     callcnt = 10;
   }
   if ( receive ) {
       fprintf(stderr, "%12d %12d %12d\n",
               frmrcvd, rcvdgood, rcvdbad);
   } else {
       fprintf(stderr, "%12d %12d %12d\n",
               frmsent, nintr, nnobufs);
   }
   time(&lastout);
  }

}

void sigterm(int signo)
{
  fprintf(stderr, "-------------------------------------------\n");
  showstats(greceive, 0);
  running = 0;
}

int main(int argc, char **argv)
{
  struct ifreq ifr;
  struct sockaddr_can addr;
  struct can_frame frame = {
    .can_dlc = 8,
  };
  struct can_filter filter[] = {
    {
      .can_id = CAN_ID_DEFAULT,
    },
  };
  char *interface = "can0";
  uint64_t seq = 0;
  uint64_t rcvseq = 0;
  int seq_wrap = 0;
  int family = PF_CAN, type = SOCK_RAW, proto = CAN_RAW;
  int loopcount = 1, infinite = 1;
  int sleepus  = 0;
  int use_poll = 0;
  int extended = 0;
  int statintv = -1;

  int nbytes;
  int opt;
  int receive  = 0;
  int seq_init = 1;
  int verbose  = 0, quit = 0;
  int exit_value = EXIT_SUCCESS;
  time_t t;
  struct tm *lt;
  char timestr[64];

  signal(SIGINT, sigterm);
  signal(SIGTERM, sigterm);
  signal(SIGHUP, sigterm);

  struct option long_options[] = {
    { "extended",   no_argument,        0, 'e' },
    { "help",       no_argument,        0, 'h' },
    { "poll",       no_argument,        0, 'p' },
    { "quit",       no_argument,        0, 'q' },
    { "receive",    no_argument,        0, 'r' },
    { "verbose",    no_argument,        0, 'v' },
    { "version",    no_argument,        0, VERSION_OPTION},
    { "identifier", required_argument,  0, 'i' },
    { "loop",       required_argument,  0, 'l' },
    { "sleep",      required_argument,  0, 's' },
    { "stat",       required_argument,  0, 't' },
    { 0, 0, 0, 0},
  };

  while ( (opt=getopt_long(argc, argv, "ehpqrvi:l:s:t:", long_options, NULL)) 
          != -1) {
    switch (opt) {
    case 'e':
      extended = 1;
      break;

    case 'h':
      print_usage(basename(argv[0]));
      exit(EXIT_SUCCESS);
      break;

    case 'p':
      use_poll = 1;
      break;

    case 'q':
      quit = 1;
      break;

    case 'r':
      receive = 1;
      greceive = 1;
      break;

    case 'v':
      verbose++;
      break;

    case VERSION_OPTION:
      printf("canseq %s\n", "VERSION not defined");
      exit(EXIT_SUCCESS);
      break;

    case 'l':
      if (optarg) {
        loopcount = strtoul(optarg, NULL, 0);
        infinite = 0;
      } else
        infinite = 1;
      break;

    case 's':
      if (optarg) {
        sleepus = strtoul(optarg, NULL, 0);
      } else {
        sleepus = 0;
      }
      break;

    case 't':
      if (optarg) {
        statintv = strtoul(optarg, NULL, 0);
      } else {
        statintv = 0;
      }
      break;

    case 'i':
      filter->can_id = strtoul(optarg, NULL, 0);
      break;

    default:
      fprintf(stderr, "Unknown option %c\n", opt);
      break;
    }
  }

  if (argv[optind] != NULL)
    interface = argv[optind];

  if (extended) {
    filter->can_mask = CAN_EFF_MASK;
    filter->can_id  &= CAN_EFF_MASK;
    filter->can_id  |= CAN_EFF_FLAG;
  } else {
    filter->can_mask = CAN_SFF_MASK;
    filter->can_id  &= CAN_SFF_MASK;
  }
  frame.can_id = filter->can_id;

  t = time(NULL);
  lt = localtime(&t);
  strftime(timestr, 64, "%F %T", lt);
  printf("%s interface = %s, family = %d, type = %d, proto = %d\n",
         timestr, interface, family, type, proto);

  s = socket(family, type, proto);
  if (s < 0) {
    perror("socket");
    return 1;
  }

  addr.can_family = family;
  strncpy(ifr.ifr_name, interface, sizeof(ifr.ifr_name));
  if (ioctl(s, SIOCGIFINDEX, &ifr)) {
    perror("ioctl");
    return 1;
  }
  addr.can_ifindex = ifr.ifr_ifindex;

  /* first don't recv. any msgs */
  if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0)) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  if (receive) {
    /* enable recv. now */
    if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, filter, sizeof(filter))) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    struct timeval to;
    to.tv_sec  = 1;
    to.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               (void*)&to, sizeof(struct timeval));


    while ((infinite || loopcount--) && running) {
      nbytes = read(s, &frame, sizeof(struct can_frame));
      if (nbytes < 0) {
        if ( errno==EAGAIN ) {
          showstats(receive, statintv);
          continue;
        }
        else if ( errno==EINTR ) {
          continue;
        }
        else {
          perror("read");
          return 1;
        }
      }


      if (seq_init) {
        seq_init = 0;
        U8TO64(seq, frame.data);
      }

      U8TO64(rcvseq, frame.data);
      if (verbose > 1) {
        t = time(NULL);
	lt = localtime(&t);
	strftime(timestr, 64, "%F %T", lt);
        printf("%s received frame. seq number: %"PRIx64"\n", timestr, rcvseq);
      }


      if (rcvseq != seq) {
        t = time(NULL);
	lt = localtime(&t);
	strftime(timestr, 64, "%F %T", lt);
        printf("%s received wrong seq count. "
               "expected: %"PRIx64", got: %"PRIx64" missing: %d\n",
               timestr, seq, rcvseq, (int)(rcvseq-seq));
        if (quit) {
          exit_value = EXIT_FAILURE;
          break;
        }
        seq = rcvseq;
        rcvdgood = 0;
        rcvdbad++;
      }
      else { 
        rcvdgood++;
      }

      frmrcvd++;
      showstats(receive, statintv);
      seq++;
      if (verbose && !seq) {
        t = time(NULL);
	lt = localtime(&t);
	strftime(timestr, 64, "%F %T", lt);
        printf("%s seq wrap around (%d)\n", timestr, seq_wrap++);
      }

    }
  } else {
    while ((infinite || loopcount--) && running) {
      ssize_t len;

      if (verbose > 1) {
        t = time(NULL);
	lt = localtime(&t);
	strftime(timestr, 64, "%F %T", lt);
        printf("%s sending frame. seq number: %"PRIx64"\n", timestr, seq);
      }


    again:
      showstats(receive, statintv);
      if ( sleepus > 0 ) usleep(sleepus);
      len = write(s, &frame, sizeof(frame));
      if (len == -1) {
        switch (errno) {
        case ENOBUFS: {
          int err;
          struct pollfd fds[] = {
            {
              .fd = s,
              .events = POLLOUT,
            },
          };

          nnobufs++;
          if (!use_poll) {
            perror("write");
            exit(EXIT_FAILURE);
          }

          err = poll(fds, 0, 10);
          if (err == -1 && errno != EINTR) {
            perror("poll");
            exit(EXIT_FAILURE);
          }
          goto again;
        }

        case EINTR:
          nintr++;
          goto again;
        default:
          perror("write");
          exit(EXIT_FAILURE);
        }
      }
      else {
       frmsent++;
      }

      seq++;
            U64TO8(frame.data, seq);

      if (verbose && !seq) {
        t = time(NULL);
	lt = localtime(&t);
	strftime(timestr, 64, "%F %T", lt);
        printf("%s seq wrap around (%d)\n", timestr, seq_wrap++);
      }
    }
  }

  exit(exit_value);
}
