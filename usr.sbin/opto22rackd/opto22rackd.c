#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <libgpio.h>

#include "opto22rackd.h"

typedef struct mcan_s mcan_t;

// this is sort of like, but not the same as SocketCAN 'struct can_frame'
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  _pad[3];
    uint8_t  data[8];
} mcan_msg_t;

// Multicast CAN frame receive callback and transmit function prototypes
typedef void(*mcan_rx)(mcan_t*, mcan_msg_t*);
int mcan_tx(mcan_t *mcan, mcan_msg_t *msg);

struct mcan_s {
    int fd;
    mcan_rx   rx_func;

    // app context
    void *arg;

    struct addrinfo tx_addrinfo;
};

int
mcan_tx(mcan_t *mcan, mcan_msg_t *msg)
{
    int n;
    mcan_msg_t txm = *msg;
    // TODO: Convert byte order...
    
    n = sendto(mcan->fd, &txm, sizeof(txm), 0,
            mcan->tx_addrinfo.ai_addr, mcan->tx_addrinfo.ai_addrlen);

    if (n < 1) {
        fprintf(stderr,
            "Error sending: %s\n", strerror(errno));
            return 1;
    }

    return 0;
}


static void
show_help()
{
    fprintf(stdout,
        "Usage: -h -i interface\n"
        "  -h --help            Show this help\n"
        "  -i --interface       Select network interface name: default none\n"
        "  -m --msgid           Integer message id: default 10\n"
        "  -p --period          Transmit interval (mS): default 100mS\n"
        "  -l --led             Blink led indicator: default none\n"
        "  -f --fg              Don't daemonize, run in forground\n"
        "\n"
    );
}

mcan_t *
mcan_new_tx(const char *ifname);

/* Instantiate datagram socket capable of TX Multicast */
mcan_t *
mcan_new_tx(const char *ifname)
{
    int s, rc;
    unsigned int ifindex = 0;
    struct addrinfo hints, *res;

    mcan_t *m = malloc(sizeof(mcan_t));
    if (m == NULL)
        return NULL;

    memset(m, 0, sizeof(mcan_t));
    m->fd = -1;


    /* Must have interface index */
    ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "Invalid interface: %s\n", ifname);
        exit(1);
    }

    s = socket(PF_INET6, SOCK_DGRAM, 0);
    if (s < 1) {
        fprintf(stderr, "Unable to open tx socket: %s\n", strerror(errno));
        exit(1);
    }

    rc = setsockopt(s,
                IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    if (rc != 0) {
        fprintf(stderr,
            "Error setting tx socket option: %s\n", strerror(errno));
        exit(1);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    getaddrinfo(MCAN_ADDR_LINK, MCAN_UDP_PORT, &hints, &res);

    m->fd = s;
    m->tx_addrinfo = *res;
    freeaddrinfo(res);

    return m;
}

int read_g4(int rack, int io);

/* return < 0 on error or 0|1 depending on I/O point off | on */
int
read_g4(int rack, int io)
{
    char val = 0;
    char name[32];
    snprintf(name, sizeof(name), "/dev/g4-%d.%d", rack, io);
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return -1;
    
    int n = read(fd, &val, sizeof(val));

    if (n != sizeof(val)) {
        close(fd);
        return -1;
    }
    close(fd);

    if (val == '1' | val == 1)
        return 1;

    return 0;
}

int
main(int argc, char *argv[])
{
    int c;
    gpio_handle_t g = GPIO_INVALID_HANDLE;

    const char *mc_iface = NULL;
    uint32_t msgid = 10;
    int period = 100 * 1000;
    int fg = 0;
    int led = -1;

    int option_index = 0;
    struct option long_options[] = {
        { "help",      no_argument,       0, 'h'  },
        { "interface", required_argument, 0, 'i'  },
        { "msgid",     required_argument, 0, 'm'  },
        { "period",    required_argument, 0, 'p'  },
        { "fg",        no_argument,       0, 'f'  },
        { "led",       required_argument, 0, 'l'  },
        { NULL,        0,                 0, '\0' }
    };

    while ((c = getopt_long(argc, argv,
        "hi:m:p:fl:", long_options, &option_index)) != -1)
    {
        switch (c) {
        case 'h':
            show_help();
            return 0;

        case 'i':
            mc_iface = optarg;
            break;

    case 'm':
        msgid = atoi(optarg);
        break;

    case 'p':
        period = atoi(optarg) * 1000;
        break;

    case 'l':
        led = atoi(optarg);
        break;

    case 'f':
        fg = 1;
        break;

        default:
            break;
        }
    }

    mcan_t *mcan = mcan_new_tx(mc_iface);

    if (mcan == NULL) {
        fprintf(stderr, "Unable to allocate multicast CAN object\n");
        exit(1);
    }

    if (led > 0) {
        g = gpio_open(0);
        if (g == GPIO_INVALID_HANDLE) {
            fprintf(stderr, "Could not open gpio device\n");
            led = -1;
        }
    }

    if (fg == 0) {
        fprintf(stderr, "Daemonizing...\n");
        daemon(0, 0);
    }

    while (1) {
        int n;
        uint32_t iosts = 0;
        mcan_msg_t msg;
        msg.id = msgid;
        msg.dlc = sizeof(iosts);
        for (n = 0; n < 24; n++) {
            if (read_g4(0, n) == 1)
                iosts |= (1 << n);
        }
        
        /* little endian */
        msg.data[0] = (uint8_t)(iosts);
        msg.data[1] = (uint8_t)(iosts >> 8);
        msg.data[2] = (uint8_t)(iosts >> 16);
        msg.data[3] = (uint8_t)(iosts >> 24);
        mcan_tx(mcan, &msg);

        if (led > 0)
            gpio_pin_toggle(g, (gpio_pin_t)led);
        
        usleep(period);
    }

    return 0;
}

