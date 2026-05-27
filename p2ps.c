#include <stdio.h>         
#include <stdlib.h>             
#include <string.h>         
#include <unistd.h>         
#include <arpa/inet.h>          
#include <sys/socket.h>         

#define DEFAULT_UDP_PORT 4000

#define BUFLEN           100    // Maximum length of payload in PDU->data 
#define FIELDLEN         100    // Length of fixed-size peer/content fields 
#define MAX_ENTRIES      512    // Maximum number of entries in the registry table 

/* Packed PDU structure for all messages (no padding bytes) */
#pragma pack(push, 1)
typedef struct {
    char     type;                  /* PDU type: 'R','S','T','O','Q','A','E','F' */
    char     peer[FIELDLEN];        /* peer name (space padded) */
    char     content[FIELDLEN];     /* content name (space padded) */
    unsigned short len;             /* length of data[] field in network byte order */
    char     data[BUFLEN];          /* payload data (text) */
} PDU;
#pragma pack(pop)

/* One row in the index server registry table */
typedef struct {
    int      in_use;                     /* 0 = free, 1 = occupied */
    char     peer[FIELDLEN + 1];         /* peer name as normal C string */
    char     content[FIELDLEN + 1];      /* content name as C string */
    char     ip[INET_ADDRSTRLEN];        /* IPv4 string, e.g. "10.1.1.10" */
    unsigned short port;                 /* TCP port of content server */
    unsigned int   used;                 /* how many times this entry chosen (for load balance) */
} Entry;

/* The global table of registered content */
static Entry table[MAX_ENTRIES];

/* Convert fixed FIELDLEN buffer with padding spaces into normal string */
static void trim_field(const char src[FIELDLEN], char *dst)
{
    int i, n;
    /* find index of last non-space character, scanning backwards */
    for (i = FIELDLEN - 1; i >= 0 && src[i] == ' '; --i) { }
    n = i + 1;                   /* length of actual content */
    if (n < 0) n = 0;            /* safety */
    memcpy(dst, src, n);         /* copy that many bytes */
    dst[n] = '\0';               /* null terminate */
}

/* Opposite of trim_field: copy src and pad with spaces to FIELDLEN */
static void pad_field(const char *src, char dst[FIELDLEN])
{
    size_t n = strlen(src);          /* length of source string */
    if (n > FIELDLEN) n = FIELDLEN;  /* clamp to FIELDLEN */
    memset(dst, ' ', FIELDLEN);      /* fill with spaces */
    memcpy(dst, src, n);             /* copy actual chars at front */
}

/* Clear a PDU to all zeros */
static void pdu_clear(PDU *p)
{
    memset(p, 0, sizeof(*p));
}

/* Find exact table entry that matches peer, content, ip, and port */
static int find_entry(const char *peer, const char *content,
                      const char *ip, unsigned short port)
{
    int i;
    for (i = 0; i < MAX_ENTRIES; ++i) {          /* scan table */
        if (!table[i].in_use) continue;          /* skip empty rows */
        /* must match all four fields */
        if (strcmp(table[i].peer,    peer)    == 0 &&
            strcmp(table[i].content, content) == 0 &&
            strcmp(table[i].ip,      ip)      == 0 &&
            table[i].port == port) {
            return i;                             /* found index */
        }
    }
    return -1;                                   /* not found */
}

/* Check if this content is registered by any peer at all */
static int is_content_registered_by_anyone(const char *content)
{
    int i;
    for (i = 0; i < MAX_ENTRIES; ++i) {
        if (!table[i].in_use) continue;          /* skip free rows */
        if (strcmp(table[i].content, content) == 0) {
            return 1;                            /* found at least one */
        }
    }
    return 0;                                    /* not found in table */
}

/* Add a new entry to registry table if there is free space */
static int add_entry(const char *peer, const char *content,
                     const char *ip, unsigned short port)
{
    int i;
    /* avoid duplicates: check if exact entry already exists */
    if (find_entry(peer, content, ip, port) >= 0) return 0;  /* 0 = already exists */

    /* search for first free slot in table */
    for (i = 0; i < MAX_ENTRIES; ++i) {
        if (!table[i].in_use) {                       /* found a free row */
            table[i].in_use = 1;                      /* mark as occupied */

            /* copy peer name, ensure null-terminated */
            strncpy(table[i].peer, peer, FIELDLEN);
            table[i].peer[FIELDLEN] = '\0';

            /* copy content name, ensure null-terminated */
            strncpy(table[i].content, content, FIELDLEN);
            table[i].content[FIELDLEN] = '\0';

            /* copy IP address string, ensure null-terminated */
            strncpy(table[i].ip, ip, sizeof(table[i].ip) - 1);
            table[i].ip[sizeof(table[i].ip) - 1] = '\0';

            table[i].port = port;                     /* store TCP port */
            table[i].used = 0;                        /* reset usage count */
            return 1;                                 /* success */
        }
    }
    return -1;                                        /* table is full */
}

/* Remove entries that match (peer, content). Return count removed. */
static int remove_entry_one(const char *peer, const char *content)
{
    int i, removed = 0;
    for (i = 0; i < MAX_ENTRIES; ++i) {
        if (!table[i].in_use) continue;              /* skip empty rows */
        /* both peer and content must match */
        if (strcmp(table[i].peer, peer) == 0 &&
            strcmp(table[i].content, content) == 0) {
            table[i].in_use = 0;                     /* mark row as free */
            removed++;                               /* count removal */
        }
    }
    return removed;                                  /* number of rows removed */
}

/* Remove all entries owned by this peer. Return count removed. */
static int remove_entry_all_for_peer(const char *peer)
{
    int i, removed = 0;
    for (i = 0; i < MAX_ENTRIES; ++i) {
        if (!table[i].in_use) continue;              /* skip free */
        if (strcmp(table[i].peer, peer) == 0) {      /* match peer name */
            table[i].in_use = 0;                     /* free row */
            removed++;                               /* count */
        }
    }
    return removed;
}

/* Choose which content server to use for a content (least-used policy) 2b)*/
static int choose_server_for_content(const char *content)
{
    int i, choice = -1;
    unsigned int best = 0xFFFFFFFFu;                 /* large starting value */

    for (i = 0; i < MAX_ENTRIES; ++i) {
        if (!table[i].in_use) continue;              /* skip empty rows */
        if (strcmp(table[i].content, content) == 0) {/* same content name */
            /* choose entry with smallest 'used' counter */
            if (table[i].used < best) {
                best = table[i].used;
                choice = i;                          /* remember index */
            }
        }
    }
    return choice;                                   /* -1 if none */
}

/* ----------------------------- main ----------------------------- */

int main(int argc, char **argv)
{
    int udp_port, sock, i;
    struct sockaddr_in srv;      /* server address for bind() */
    struct sockaddr_in cli;      /* client address from recvfrom() */
    socklen_t clen;

    /* If user supplies a port, use it; otherwise default to 4000 */
    if (argc >= 2) udp_port = atoi(argv[1]);
    else udp_port = DEFAULT_UDP_PORT;

    /* Create UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    /* Prepare server address structure */
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;              /* IPv4 */
    srv.sin_addr.s_addr = INADDR_ANY;      /* bind all local interfaces */
    srv.sin_port = htons(udp_port);        /* server UDP port */

    /* Bind socket to address and port */
    if (bind(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind");
        exit(1);
    }

    printf("Index server listening on UDP %d\n", udp_port);

    /* Clear registry table at startup */
    memset(table, 0, sizeof(table));
    clen = sizeof(cli);                    /* size of client addr struct */

    /* Main server loop: receive request, handle, send reply */
    for (;;) {
        PDU in, out;                       /* incoming and outgoing PDUs */
        char peer[FIELDLEN+1];             /* trimmed peer name */
        char content[FIELDLEN+1];          /* trimmed content name */
        char cli_ip[INET_ADDRSTRLEN];      /* client IP as string */
        ssize_t r;                         /* bytes received */
        int idx;                           /* table index */
        unsigned short L;                  /* payload length */
        unsigned short port;               /* TCP port from client */
        char msg[64];                      /* small message buffer */

        /* Initialise PDUs and helper buffers for each request */
        pdu_clear(&in);
        pdu_clear(&out);
        memset(peer, 0, sizeof(peer));
        memset(content, 0, sizeof(content));
        memset(cli_ip, 0, sizeof(cli_ip));

        /* Receive PDU from some peer 2a) */
        r = recvfrom(sock, &in, sizeof(in), 0, // recvfrom gets the R PDU from some client
                     (struct sockaddr*)&cli, &clen); // cli holds the source UDP address -> used cli.sin_addr as the peer's IP
        if (r <= 0) continue;              /* ignore errors/timeouts */

        /* Convert peer and content fields from padded to normal strings */
        trim_field(in.peer, peer);
        trim_field(in.content, content);

        /* Convert client IP address from binary to dotted-decimal string */
        inet_ntop(AF_INET, &cli.sin_addr, cli_ip, sizeof(cli_ip));

        /* Decide what to do based on PDU type 2a) */
        switch (in.type) {    // when in.type == 'R'

        case 'R': {            /* Handles Register */
            char buf[BUFLEN+1];
            int rc;

            /* Get length of port string from PDU, convert from network order */
            L = ntohs(in.len);
            memset(buf, 0, sizeof(buf));
            /* Copy that many bytes from data into buf */
            if (L > 0 && L <= BUFLEN) memcpy(buf, in.data, L);
            /* Convert port text to number */
            port = (unsigned short)atoi(buf);

            /* Add entry to table: peer, content, client IP, TCP port */
            rc = add_entry(peer, content, cli_ip, port);

            /* Prepare reply: A for success, E for error (table full) */
            out.type = (rc < 0) ? 'E' : 'A';
            if (rc < 0) strcpy(msg, "Registry full");
            else strcpy(msg, "Registered");
            out.len = htons((unsigned short)strlen(msg));
            memcpy(out.data, msg, strlen(msg));

            /* Send reply back to peer */
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
        } break;

        case 'S': { /* Search 2b) */
            char ipport[64];   /* will hold "ip:port" string */

            /* select which content server to use */
            idx = choose_server_for_content(content);

            if (idx < 0) {
                /* No peer has this content: send error */
                out.type = 'E';
                sprintf(msg, "Error: '%s' is not registered", content);
                out.len = htons((unsigned short)strlen(msg));
                memcpy(out.data, msg, strlen(msg));
            } else {
                /* Build "ip:port" string from chosen entry */
                sprintf(ipport, "%s:%u",
                        table[idx].ip, (unsigned)table[idx].port);
                table[idx].used++; /* increment usage counter */

                out.type = 'S';   /* reply is S-type on success */
                out.len = htons((unsigned short)strlen(ipport));
                memcpy(out.data, ipport, strlen(ipport));
            }
            /* Echo back the peer and content fields (optional) */
            pad_field(peer, out.peer);
            pad_field(content, out.content);

            /* Send reply to searching peer */
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
        } break;

        case 'T': { /* De-register one content for this peer */
            int n = remove_entry_one(peer, content);
            if (n > 0) {
                /* At least one entry removed: success */
                out.type = 'A';
                sprintf(msg, "Success: Deregistered '%s'", content);
            } else {
                /* No entry removed: check why */
                if (is_content_registered_by_anyone(content)) {
                    /* Someone else has this content -> permission error */
                    out.type = 'E';
                    sprintf(msg,
                            "Error: Permission denied. You did not register '%s'.",
                            content);
                } else {
                    /* Content does not exist at all in table */
                    out.type = 'E';
                    sprintf(msg,
                            "Error: '%s' is not registered on the server.",
                            content);
                }
            }
            out.len = htons((unsigned short)strlen(msg));
            memcpy(out.data, msg, strlen(msg));

            /* Send reply to peer */
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
        } break;

        case 'Q': { /* Quit: remove all entries belonging to this peer */
            int n = remove_entry_all_for_peer(peer); /* delete rows for this peer */
            out.type = 'A';                          /* A = success */
            sprintf(msg, "Removed %d", n);           /* how many removed */
            out.len = htons((unsigned short)strlen(msg));
            memcpy(out.data, msg, strlen(msg));

            /* Send reply */
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
        } break;

        case 'O': { /* List all entries to requesting peer */
            for (i = 0; i < MAX_ENTRIES; ++i) {
                char ipport[64];
                if (!table[i].in_use) continue;     /* skip free rows */

                pdu_clear(&out);
                out.type = 'O';                     /* 'O' = one list entry */

                /* Put peer name and content into PDU fields */
                pad_field(table[i].peer,    out.peer);
                pad_field(table[i].content, out.content);

                /* Format address as "ip:port" */
                sprintf(ipport, "%s:%u",
                        table[i].ip, (unsigned)table[i].port);

                /* Put "ip:port" into data field */
                out.len = htons((unsigned short)strlen(ipport));
                memcpy(out.data, ipport, strlen(ipport));

                /* Send this entry to requesting peer */
                sendto(sock, &out, sizeof(out), 0,
                       (struct sockaddr*)&cli, clen);
            }

            /* After sending all entries, send an 'F' PDU to mark the end */
            pdu_clear(&out);
            out.type = 'F';                    /* 'F' = finished listing */
            strcpy(msg, "List finished");
            out.len = htons((unsigned short)strlen(msg));
            memcpy(out.data, msg, strlen(msg));
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
        } break;

        default:
            /* Any unknown PDU type: send generic error */
            out.type = 'E';
            strcpy(msg, "Unknown type");
            out.len = htons((unsigned short)strlen(msg));
            memcpy(out.data, msg, strlen(msg));
            sendto(sock, &out, sizeof(out), 0,
                   (struct sockaddr*)&cli, clen);
            break;
        } 
    } 

    return 0; /* never reached in normal execution */
}
