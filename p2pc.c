#include <stdio.h>      
#include <stdlib.h>     
#include <string.h>     
#include <unistd.h>     
#include <signal.h>     
#include <arpa/inet.h>  
#include <sys/socket.h> 
#include <sys/types.h> 
#include <sys/wait.h> 

#define DEFAULT_UDP_PORT 4000   /* default index server UDP port */
#define DEFAULT_TCP_PORT 5001   /* default TCP port for hosting content */
#define BUFLEN           100    /* max payload size in PDU data field */
#define FIELDLEN         100    /* length of peer/content fixed fields */

/* Make sure the compiler does not add padding inside this struct */
#pragma pack(push, 1)
typedef struct {
    char     type;                 /* PDU type: 'R','S','T','O','Q','D','C','F','E' etc. */
    char     peer[FIELDLEN];       /* peer name, space padded */
    char     content[FIELDLEN];    /* content name, space padded */
    unsigned short len;            /* length of data[] in network byte order */
    char     data[BUFLEN];         /* payload data */
} PDU;
#pragma pack(pop)

/* Helper to get clean input from user without trailing newline */
static void get_input(const char *prompt, char *buffer, size_t size) {
    printf("%s", prompt);                      /* show prompt text */
    if (fgets(buffer, size, stdin) != NULL) {  /* read a line from stdin */
        size_t len = strlen(buffer);           /* get length of input line */
        if (len > 0 && buffer[len-1] == '\n') {/* if last char is newline */
            buffer[len-1] = '\0';              /* replace newline with null */
        }
    } else {
        buffer[0] = '\0';                      /* if fgets fails, set empty string */
    }
}

/* Copy src into dst, pad with spaces to FIELDLEN */
static void pad_field(const char *src, char dst[FIELDLEN])
{
    size_t n = strlen(src);           /* length of source string */
    if (n > FIELDLEN) n = FIELDLEN;   /* limit to FIELDLEN chars */
    memset(dst, ' ', FIELDLEN);       /* fill entire field with spaces */
    memcpy(dst, src, n);              /* copy actual characters at start */
}

/* Trim trailing spaces from fixed field into normal C string */
static void trim_field(const char src[FIELDLEN], char *dst)
{
    int i, n;
    /* move from end of field backwards until non-space is found */
    for (i = FIELDLEN - 1; i >= 0 && src[i] == ' '; --i) { }
    n = i + 1;                         /* number of real characters */
    if (n < 0) n = 0;                  /* safety check */
    memcpy(dst, src, n);               /* copy that many characters */
    dst[n] = '\0';                     /* null-terminate result */
}

/* Clear a PDU struct to all zeros */
static void pdu_clear(PDU *p)
{
    memset(p, 0, sizeof(*p));          /* set all bytes to 0 */
}

/* Read exactly n bytes from fd into buf, unless error/EOF */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;                    /* how many bytes have been read */
    while (got < n) {                  /* loop until we got n bytes */
        ssize_t r = read(fd, (char*)buf + got, n - got); /* read remaining bytes */
        if (r <= 0) return r;          /* error or EOF: stop and return */
        got += (size_t)r;              /* increase count by bytes read */
    }
    return (ssize_t)got;               /* return total bytes read */
}

/* Write exactly n bytes from buf to fd, unless error */
static ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t sent = 0;                   /* how many bytes have been sent */
    while (sent < n) {                 /* loop until n bytes are sent */
        ssize_t w = write(fd, (const char*)buf + sent, n - sent); /* write remaining */
        if (w <= 0) return w;          /* error: stop and return */
        sent += (size_t)w;             /* increase count by bytes written */
    }
    return (ssize_t)sent;              /* return total bytes sent */
}

/* Create a UDP socket for talking to the index server */
static int udp_socket_to_index(const char *index_ip, int index_port,
                               struct sockaddr_in *idx)
{
    struct timeval tv;                           /* for receive timeout */
    int s = socket(AF_INET, SOCK_DGRAM, 0);      /* create UDP socket */
    if (s < 0) { perror("socket UDP"); exit(1); }

    /* Set Timeout (5 seconds) to prevent freezing on recvfrom */
    tv.tv_sec = 5;                               /* 5 second timeout */
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(idx, 0, sizeof(*idx));               /* clear sockaddr_in */
    idx->sin_family = AF_INET;                  /* IPv4 */
    idx->sin_port   = htons(index_port);        /* index server UDP port */

    /* convert IP from text to binary form */
    if (inet_pton(AF_INET, index_ip, &idx->sin_addr) != 1) {
        fprintf(stderr, "Error: Bad index IP address\n");
        exit(1);
    }

    return s;                                   /* return UDP socket fd */
}

/* Set up a TCP listening socket for this peer's content server */
static int tcp_listen_port(int my_tcp_port)
{
    int s, one = 1;
    struct sockaddr_in me;

    s = socket(AF_INET, SOCK_STREAM, 0);        /* create TCP socket */
    if (s < 0) { perror("socket TCP"); exit(1); }

    /* allow immediate reuse of address after program restart */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&me, 0, sizeof(me));                 /* clear sockaddr_in */
    me.sin_family      = AF_INET;               /* IPv4 */
    me.sin_addr.s_addr = INADDR_ANY;            /* listen on all interfaces */
    me.sin_port        = htons(my_tcp_port);    /* TCP port for this peer */

    if (bind(s, (struct sockaddr*)&me, sizeof(me)) < 0) {
        perror("bind TCP");
        exit(1);
    }
    if (listen(s, 16) < 0) {                    /* start listening */
        perror("listen TCP");
        exit(1);
    }
    return s;                                   /* return listening socket fd */
}

/* Serve one TCP download request from another peer (content server peer) 2b)*/
static void serve_one_download(int cfd)
{
    PDU in, out;
    FILE *fp;
    unsigned short L;
    char filename[FIELDLEN+1];

    /* read the request PDU */
    if (read_full(cfd, &in, sizeof(in)) <= 0) { close(cfd); return; }
    L = ntohs(in.len);    /* not really used, but decode length */
    (void)L;              /* avoid unused variable warning */

    /* only handle type 'D' (Download request) */
    if (in.type != 'D') { close(cfd); return; }

    /* extract the requested file name */
    trim_field(in.content, filename);

    /* open that file for reading */
    fp = fopen(filename, "rb");
    if (!fp) {
        /* if file does not exist, send error PDU */
        pdu_clear(&out);
        out.type = 'E';
        strcpy(out.data, "File Not Found");
        out.len = htons((unsigned short)strlen(out.data));
        write_full(cfd, &out, sizeof(out));
        close(cfd);
        return;
    }

    /* send file in chunks of size BUFLEN */
    for (;;) {
        size_t n;
        pdu_clear(&out);                   /* clear PDU */
        n = fread(out.data, 1, BUFLEN, fp);/* read next chunk from file */

        if (n > 0) {
            out.type = 'C';                /* 'C' = content chunk */
            out.len  = htons((unsigned short)n);
            if (write_full(cfd, &out, sizeof(out)) <= 0) break;
        }

        if (n < BUFLEN) {                  /* last chunk (or error) */
            pdu_clear(&out);
            out.type = 'F';                /* 'F' = finished */
            out.len  = htons(0);
            write_full(cfd, &out, sizeof(out));
            break;
        }
    }

    fclose(fp);                            /* close the file */
    close(cfd);                            /* close TCP connection */
}

/* Loop accepting incoming TCP connections to download content 2b)*/
static void accept_loop(int tcpfd)
{
    for (;;) {                                     /* run forever */
        struct sockaddr_in cli;                    /* client address */
        socklen_t clen = sizeof(cli);
        int cfd = accept(tcpfd, (struct sockaddr*)&cli, &clen); /* accept new conn */
        if (cfd < 0) continue;                     /* ignore errors */

        if (fork() == 0) {                         /* child process handles client */
            close(tcpfd);                          /* child does not need listen fd */
            serve_one_download(cfd);               /* send file to client */
            _exit(0);                              /* exit child when done */
        } else {
            close(cfd);                            /* parent closes connected fd */
            waitpid(-1, NULL, WNOHANG);            /* reap any finished children */
        }
    }
}

/* Send one PDU over UDP to index server */
static void send_udp(int usock, struct sockaddr_in *idx, const PDU *out)
{
    sendto(usock, out, sizeof(*out), 0, (struct sockaddr*)idx, sizeof(*idx));
}

/* Receive one PDU over UDP from index server */
static int recv_udp(int usock, PDU *in, struct sockaddr_in *from)
{
    socklen_t flen = sizeof(*from);
    ssize_t r = recvfrom(usock, in, sizeof(*in), 0,
                         (struct sockaddr*)from, &flen);
    if (r <= 0) return -1;     /* error or timeout */
    return 0;                  /* success */
}

/* Register content with index server, after checking file exists 2a) */
static void do_register(int usock, struct sockaddr_in *idx,
                        const char *peer, const char *content,
                        int my_tcp_port)
{
    PDU out, in;
    char porttxt[16];
    size_t L;
    FILE *fp;

    /* verify the file exists locally */
    fp = fopen(content, "rb");
    if (fp == NULL) {
        printf("Error: Cannot register '%s'. File does not exist locally.\n", content);
        return;
    }
    fclose(fp);

    /* build an R-type PDU */
    pdu_clear(&out);
    out.type = 'R';                        /* 'marks it as a Register message */
    pad_field(peer, out.peer);             /* store peer name in fixed-length field */
    pad_field(content, out.content);       /* store content name */

    /* put TCP port number into data as text */
    sprintf(porttxt, "%d", my_tcp_port);
    L = strlen(porttxt);
    memcpy(out.data, porttxt, L);
    out.len = htons((unsigned short)L);    /* store length of port string */

    /* send R PDU to index server */
    send_udp(usock, idx, &out);

    /* wait for reply */
    if (recv_udp(usock, &in, idx) == 0 && in.type == 'A') //recv waits for reply, 'A' type -> success, else error message
        printf("Success: Registered content '%s'.\n", content);
    else
        printf("Error: Registration failed or timed out.\n");
}

/* Ask index server who hosts a given content 2b)*/
static int do_search(int usock, struct sockaddr_in *idx,
                     const char *peer, const char *content,
                     char ip[INET_ADDRSTRLEN], int *port)
{
    PDU out, in;
    char buf[BUFLEN+1];
    unsigned short L;
    char *colon;

    /* build S-type PDU */
    pdu_clear(&out);
    out.type = 'S';                        /* 'S' = search */
    pad_field(peer, out.peer);             /* peer name */
    pad_field(content, out.content);       /* content name */
    out.len = htons(0);                    /* no data payload */
    send_udp(usock, idx, &out);            /* send to server 2b*/

    /* wait for response */
    if (recv_udp(usock, &in, idx) != 0) {
         printf("Error: No response from Index Server.\n");
         return -1;
    }

    /* server should answer with S-type or E-type */
    if (in.type != 'S') {
        /* E-type: print error message from server */
        L = ntohs(in.len);
        if (L > BUFLEN) L = BUFLEN;
        memcpy(buf, in.data, L);
        buf[L] = '\0';
        printf("%s\n", buf);
        return -1;
    }

    /* copy "ip:port" string from data */
    L = ntohs(in.len);
    if (L > BUFLEN) L = BUFLEN;
    memcpy(buf, in.data, L);
    buf[L] = '\0';

    /* split at ':' to separate IP (server ip string) and port (port number) */
    colon = strchr(buf, ':');
    if (!colon) return -1;                 /* malformed string */

    *colon = '\0';                         /* terminate IP string */
    strncpy(ip, buf, INET_ADDRSTRLEN - 1); /* copy IP */
    ip[INET_ADDRSTRLEN - 1] = '\0';        /* ensure null-terminated */
    *port = atoi(colon + 1);               /* parse port number */
    return 0;                              /* success, turns content name into TCP address */
}

/* Download file from another peer over TCP */
static int download_from_peer(const char *ip, int port,
                              const char *peer, const char *content,
                              const char *save_as)
{
    int s;
    struct sockaddr_in dst;
    PDU out, in;
    FILE *fp;
    unsigned short L;
    int total_bytes = 0;                   /* track how much we downloaded */

    printf("Connecting to %s:%d...\n", ip, port);

    s = socket(AF_INET, SOCK_STREAM, 0);   /* create TCP socket */
    if (s < 0) { perror("socket TCP"); return -1; }

    memset(&dst, 0, sizeof(dst));          /* clear address */
    dst.sin_family = AF_INET;              /* IPv4 */
    dst.sin_port   = htons(port);          /* remote peer TCP port */

    /* convert text IP to binary form */
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        close(s);
        return -1;
    }
    /* connect to remote content server */
    if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        perror("connect");
        close(s);
        return -1;
    }

    /* send D-type PDU requesting the content */
    pdu_clear(&out);
    out.type = 'D';                        /* 'D' = download request file */
    pad_field(peer, out.peer);             /* our peer name */
    pad_field(content, out.content);       /* requested content name */
    out.len = htons(0);                    /* no extra data */
    write_full(s, &out, sizeof(out));      /* send request over TCP */

    /* open local file for writing received data */
    fp = fopen(save_as, "wb");
    if (!fp) { perror("fopen"); close(s); return -1; }

    printf("Downloading...");
    fflush(stdout);

    /* read PDUs from TCP connection until F-type or error */
    for (;;) {
        if (read_full(s, &in, sizeof(in)) <= 0) break; /* lost connection */
        L = ntohs(in.len);
        if (L > BUFLEN) L = BUFLEN;         /* safety */

        if (in.type == 'C') {              /* content chunk */
            fwrite(in.data, 1, L, fp);     /* write bytes from data to file */
            total_bytes += L;              /* update counter */
            printf(".");                   /* show progress dot */
            fflush(stdout);
        } else if (in.type == 'F') {       /* finished */
            break;                         /* leave loop */
        } else if (in.type == 'E') {       /* error from server */
            fwrite(in.data, 1, L, stdout); /* print error message */
            fclose(fp);
            close(s);
            printf("\nPeer returned Error.\n");
            return -1;
        }
    }

    fclose(fp);                            /* close local file */
    close(s);                              /* close TCP socket */
    printf("\nDownload Complete (%d bytes) saved as '%s'.\n",
           total_bytes, save_as);
    return 0;                              /* success */
}

/* --------------- main: peer entry point --------------- */

int main(int argc, char **argv)
{
    const char *index_ip;        /* IP of index server */
    int index_port, my_tcp_port; /* UDP port, my TCP port */
    const char *peer_name;       /* this peer's name */

    struct sockaddr_in idx;      /* index server address */
    int usock, tlisten;          /* UDP socket, TCP listen socket */
    pid_t hoster;                /* PID of TCP server child */

    char line[256], a[128], b[128]; /* buffers for user input */

    /* Show usage if index IP is missing, but still go on with defaults */
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <index_ip> [index_port] [tcp_port] [peer_name]\n",
                argv[0]);
        fprintf(stderr,
                "Defaults: index_port=4000, tcp=5001, name=Peer\n");
    }

    /* read command line arguments or use defaults */
    index_ip    = (argc >= 2) ? argv[1] : "127.0.0.1";
    index_port  = (argc >= 3) ? atoi(argv[2]) : DEFAULT_UDP_PORT;
    my_tcp_port = (argc >= 4) ? atoi(argv[3]) : DEFAULT_TCP_PORT;
    peer_name   = (argc >= 5) ? argv[4] : "Peer";

    /* set up UDP socket to index server */
    usock   = udp_socket_to_index(index_ip, index_port, &idx);
    /* set up TCP listening socket for file hosting 2b) */
    tlisten = tcp_listen_port(my_tcp_port);

    /* fork TCP hosting process */
    hoster = fork();
    if (hoster == 0) {                  /* child process: TCP server */
        accept_loop(tlisten);           /* serve downloads forever */
        _exit(0);                       /* should never return */
    }

    /* parent process: interactive client */
    printf("------------------------------------------------------------\n");
    printf("   P2P FILE SHARING CLIENT  |  User: %s\n", peer_name);
    printf("------------------------------------------------------------\n");
    printf(" Index Server : %s:%d\n", index_ip, index_port);
    printf(" Local Hosting: TCP Port %d\n", my_tcp_port);
    printf("------------------------------------------------------------\n");

    for (;;) {                          /* main command loop */
        printf("\nOptions:\n");
        printf(" R - Register content\n");
        printf(" S - Search and Download content\n");
        printf(" T - Deregister content\n");
        printf(" O - List all online content\n");
        printf(" Q - Quit\n");
        
        get_input("\nEnter a command: ", line, sizeof(line)); /* read command */

        if (line[0] == '\0') continue;  /* ignore empty line */

        /* If the input is more than 1 char, treat it as invalid */
        if (strlen(line) > 1) {
            printf("Invalid command. Please enter a single character (e.g., R, S, T, O, Q).\n");
            continue;
        }

        char cmd = line[0];             /* get single-character command */

        /* ---------- R: register content ---------- 2a) */
        if (cmd == 'R' || cmd == 'r') {
            get_input("Enter file name to register: ", a, sizeof(a));
            if (strlen(a) > 0) {
                do_register(usock, &idx, peer_name, a, my_tcp_port); // calls do_register with UDP socket, index server addr, peer name, content name, TCP port
            }
        }

        /* ---------- S: search and download ---------- 2b)*/
        else if (cmd == 'S' || cmd == 's') {
            char ipbuf[INET_ADDRSTRLEN];
            int  port;
            
            get_input("Enter file name to search: ", a, sizeof(a)); /* content name */
            if (strlen(a) > 0) {
                get_input("Enter file name to save as: ", b, sizeof(b)); /* save-as name */
                if (strlen(b) > 0) {
                    /* ask index server who has this content */
                    if (do_search(usock, &idx, peer_name, a, ipbuf, &port) == 0) { // calls do_search with UDP socket, addr, name, tcp port
                        /* download from chosen peer */
                        if (download_from_peer(ipbuf, port, peer_name, a, b) == 0) { // calls download_from_peer with ip, port, peer name, content name, save-as name
                            printf("Auto-registering downloaded content...\n");
                            /* register the save-as file under this peer */
                            do_register(usock, &idx, peer_name, b, my_tcp_port);
                        } else {
                            printf("Download failed.\n");
                        }
                    }
                }
            }
        }

        /* ---------- T: de-register one content ---------- */
        else if (cmd == 'T' || cmd == 't') {
            PDU out, in;
            unsigned short L;
            char msg[BUFLEN+1];

            get_input("Enter file name to deregister: ", a, sizeof(a));
            
            if (strlen(a) > 0) {
                /* build T-type PDU */
                pdu_clear(&out);
                out.type = 'T';
                pad_field(peer_name, out.peer);
                pad_field(a, out.content);
                out.len = htons(0);
                send_udp(usock, &idx, &out);

                /* wait for response from index server */
                if (recv_udp(usock, &in, &idx) == 0) {
                    L = ntohs(in.len);
                    if (L > BUFLEN) L = BUFLEN;
                    memcpy(msg, in.data, L);
                    msg[L] = '\0';

                    if (in.type == 'E') {
                        /* error: print full message from server */
                        printf("%s\n", msg);
                    } else {
                        /* success: print type and message */
                        printf("%c: %s\n", in.type, msg);
                    }
                }
            }
        }

        /* ---------- O: list all content ---------- */
        else if (cmd == 'O' || cmd == 'o') {
            PDU out, in;
            unsigned short L;
            char p[FIELDLEN+1], c[FIELDLEN+1];
            char ipport[BUFLEN+1];
            int count = 0;

            /* send O-type request to index server */
            pdu_clear(&out);
            out.type = 'O';
            pad_field(peer_name, out.peer);
            out.len = htons(0);
            send_udp(usock, &idx, &out);

            printf("\nRegistered Content List:\n");
            
            for (;;) {
                if (recv_udp(usock, &in, &idx) != 0) break; /* timeout/error */
                if (in.type == 'F') {                       /* 'F' marks end */
                    break;
                }

                count++;                                   /* count entries */
                trim_field(in.peer, p);                    /* get peer name */
                trim_field(in.content, c);                 /* get content name */

                L = ntohs(in.len);
                if (L > BUFLEN) L = BUFLEN;
                memcpy(ipport, in.data, L);                /* copy "ip:port" */
                ipport[L] = '\0';

                printf(" %d. Peer: %s | Content: %s | Address: %s\n",
                       count, p, c, ipport);
            }

            if (count == 0) {
                printf("List is empty.\n");
            }
        }

        /* ---------- Q: quit and remove all our entries ---------- */
        else if (cmd == 'Q' || cmd == 'q') {
            PDU out, in;
            /* build Q-type PDU */
            pdu_clear(&out);
            out.type = 'Q';
            pad_field(peer_name, out.peer);
            out.len = htons(0);
            send_udp(usock, &idx, &out);

            /* server replies with how many entries removed */
            if (recv_udp(usock, &in, &idx) == 0) {
                 printf("Deregistered: %.*s\n",
                        (int)ntohs(in.len), in.data);
            }

            /* stop TCP server child process */
            kill(hoster, SIGTERM);
            waitpid(hoster, NULL, 0);
            /* close sockets */
            close(tlisten);
            close(usock);
            printf("Goodbye!\n");
            break; /* leave main loop and exit */
        }

        /* ---------- invalid command ---------- */
        else {
            printf("Unknown command.\n");
        }
    }

    return 0; /* end of program */
}
