#include <asm-generic/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <time.h>

#define BUFF_SIZE 1024
#define PORT "6969"
#define BACKLOG 10

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

char *getTimestamp(void)
{
    time_t rawtime;
    struct tm * timeinfo;
    char* a;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    a = asctime(timeinfo);
    a+=11;
    a[8] = 0;
    return a;
}

int client_setup(char *ip)
{
    int sockfd;  
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(ip, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                        p->ai_protocol)) == -1) {
            perror("client: socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("client: connect");
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        exit(2);
    }

    //inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),s, sizeof s);
    printf("client: connecting to %s\n", ip);

    freeaddrinfo(servinfo); // all done with this structure
    return sockfd;
}

void sendf(char *f_name, int sockfd)
{
    struct stat f_stat;
    int val = 1;
    char f_size[BUFF_SIZE];
    int f_name_len = strlen(f_name);
    int fd;

    if ((fd = open(f_name, O_RDONLY)) == -1) {
        printf("invalid file\n");
        exit(1);
    }

    stat(f_name, &f_stat);
    sprintf(f_size, "%ld", f_stat.st_size);
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_CORK, &val, sizeof(int *)) == -1) printf("setsockopt error\n");

    send(sockfd, &f_stat.st_size, sizeof(off_t), 0);
    send(sockfd, &f_name_len, sizeof(int), 0);
    send(sockfd, f_name, f_name_len, 0);
    //send(sockfd, "\r\n", 2, 0);
    //send(sockfd, f_size, strlen(f_size), 0);
    //send(sockfd, "\r\n", 2, 0);
    sendfile(sockfd, fd, NULL, f_stat.st_size);

    val = 0;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_CORK, &val, sizeof(int *)) == -1) printf("setsockopt error\n");
    close(fd);
}

void client(char *ip, char **files, int num_files)
{
    int fd = inotify_init();
    struct inotify_event buf[BUFF_SIZE];
    struct inotify_event *event;
    int sockfd = client_setup(ip);

    printf("\nWatching files: |");
    for (int i = 0; i < num_files; i++)
        printf(" %s |", files[i]);
    printf("\n\n");

    while (1) {
        //move this outside core loop
        //only re-add wd for the specific file that was updated
        int wds[num_files];
        for (int i  =0; i < num_files; i++)
            wds[i] = inotify_add_watch(fd, files[i], IN_IGNORED);

        int r = read(fd, buf, BUFF_SIZE);
        //check which wd was triggered
        for (int i  =0; i < num_files; i++) {
            if (buf->wd == wds[i]) {
                printf("[%s] File: %s was modified\n", getTimestamp(), files[i]);
                sendf(files[i], sockfd);
                //wds[i] = inotify_add_watch(fd, files[i], IN_IGNORED); -- this may be enough to re-init the watch
                break;
            }
        }
    }

    close(fd);
}

int server_setup()
{
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int yes = 1;
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "failed to bind socket\n");
        exit(2);
    }

    return sockfd;
}

void writeFile(char *f_name, long f_size, int sockfd)
{
    char buff[BUFF_SIZE];
    time_t rawtime;
    struct tm * timeinfo;
    long tot_bytes = 0;
    long bytes_recieved = 0;
    FILE *fp = fopen(f_name, "w");

    while (tot_bytes < f_size) {
        if ((bytes_recieved = recv(sockfd, buff, BUFF_SIZE, 0)) == -1) {
            printf("write file error\n");
            exit(1);
        }
        fwrite(buff, 1, bytes_recieved, fp);
        tot_bytes += bytes_recieved;
        memset(buff, 0, BUFF_SIZE);
    }
    fclose(fp);
    time (&rawtime);
    timeinfo = localtime(&rawtime);
    printf("[%s] File: %s Updated\n", getTimestamp(), f_name);
}

void server(void)
{
    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    int new_fd;
    char s[INET_ADDRSTRLEN];
    char request[BUFF_SIZE];
    int sockfd = server_setup();
    char f_name[BUFF_SIZE];
    off_t f_size;
    int f_name_len;
    memset(request, 0, BUFF_SIZE);
    printf("starting server...\n");

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    while (1) {
        printf("waiting for connection...\n");
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept");
            continue;
        }
        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),s, sizeof s);
        printf("connection from: %s\n", s);
        while (recv(new_fd, NULL, BUFF_SIZE, MSG_PEEK) != 0) {
            recv(new_fd, &f_size, sizeof(off_t), 0);
            recv(new_fd, &f_name_len, sizeof(int), 0);
            recv(new_fd, f_name, f_name_len, 0);
            f_name[f_name_len] = '\0';
            writeFile(f_name, f_size, new_fd);
            //printf("contains:\n%s\n", request);
            memset(request, 0, BUFF_SIZE);
            memset(f_name, 0, BUFF_SIZE);
        }
        close(new_fd);
        printf("connection closed\n");
    }
    while (1) {

    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("invalid usage\n");
        return 1;
    }
    else if (!strcmp(argv[1], "-client") && argc < 4) {
        printf("invalid usage\n");
        return 1;
    }

    int num_files = argc - 3;

    if (!strcmp(argv[1], "-client")) client(argv[2], &argv[3], num_files);
    else if (!strcmp(argv[1], "-server")) server();

    return 0;
}
