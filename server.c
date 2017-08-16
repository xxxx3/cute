#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>

#include "base64.h"
#include "cJSON.h"
#include "sha1.h"
#include "func.h"

#define DEFAULT_BUFF_SIZE 9999
#define DEFAULT_PORT 7777
#define DEFAULT_LINE_MAX 256
#define WEB_SOCKET_KEY_LEN_MAX 256

struct conn {
    int localfd;
    int remotefd;
    volatile int fin;
    pthread_mutex_t finMutex;
    pthread_cond_t finCond;
    struct sockaddr_in localAddr;
    struct sockaddr_in remoteAddr;
} conn;

typedef struct cuteServer {
    int pid;
    int fd;
    int poolSize;
    volatile int currPoolIndex;
    volatile int maxConnCount;
    volatile int currConnCount;
    pthread_t *tids;
    pthread_mutex_t connMutex;
    pthread_cond_t connEmptyCond;
    pthread_cond_t connFullCond;
    struct sockaddr_in addr;
    struct conn *pool;
    void (*runServer)(struct cuteServer *);
    void (*handleSignal)(int);
    void *(*handleConnection)(void *);
} cuteServer;

cuteServer server;
void error(char *message) {
    printf("error: %s", message);
    exit(1);
}

int getRequest(int fd, char *payloadData) {
    char buff[DEFAULT_BUFF_SIZE];
    char masks[4];
    ssize_t n = recv(fd, buff, 2, 0);
    int len = 0;
    if(fd <= 0) {
        return len;
    }
    if (n <= 0) {
        return len;
    }
    char fin = (buff[0] & 0x80) == 0x80; // 1bit，1表示最后一帧
    if (!fin) {
        return len;
    }
    char maskFlag = (buff[1] & 0x80) == 0x80; // 是否包含掩码
    if (!maskFlag) {
        return len;
    }
    int payloadLen = buff[1] & 0x7F; // 数据长度
    if (payloadLen == 126) {
        memset(buff, 0, sizeof(buff));
        n = recv(fd, buff, 6, 0);
        payloadLen = (buff[0] & 0xFF) << 8 | (buff[1] & 0xFF);
        memcpy(masks, buff + 2, 4);
        memset(buff, 0, sizeof(buff));
        n = recv(fd, buff, payloadLen, 0);
    } else if (payloadLen == 127) {
        // 有空了实现
        return len;
    } else {
        memset(buff, 0, sizeof(buff));
        n = recv(fd, buff, 4, 0);
        memcpy(masks, buff, 4);
        memset(buff, 0, sizeof(buff));
        n = recv(fd, buff, payloadLen, 0);
    }
    
    len = payloadLen;
    memset(payloadData, 0, payloadLen);
    memcpy(payloadData, buff, payloadLen);
    int i;
    for (i = 0; i < payloadLen; i++) {
        payloadData[i] = (char)(payloadData[i] ^ masks[i % 4]);
    }
    
    return len;
}

char *getSecKey(char* buff) {
    char *key;
    char *keyBegin;
    char *flag = "Sec-WebSocket-Key: ";
    int i = 0, bufLen = 0;
    
    key = (char *)malloc(WEB_SOCKET_KEY_LEN_MAX);
    memset(key, 0, WEB_SOCKET_KEY_LEN_MAX);
    if (!buff) {
        return NULL;
    }
    
    keyBegin = strstr(buff, flag);
    if (!keyBegin) {
        return NULL;
    }
    keyBegin += strlen(flag);
    
    bufLen = (int)strlen(buff);
    for (i = 0; i < bufLen; i++) {
        if (keyBegin[i] == 0x0A || keyBegin[i] == 0x0D) {
            break;
        }
        key[i] = keyBegin[i];
    }
    
    return key;
}

char *getAcceptKey(char* buff) {
    char *clientKey;
    char *serverKey;
    char *sha1DataTemp;
    char *sha1Data;
    int i, n;
    const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    
    if (!buff) {
        return NULL;
    }
    clientKey = (char *)malloc(DEFAULT_LINE_MAX);
    memset(clientKey, 0, DEFAULT_LINE_MAX);
    clientKey = getSecKey(buff);
    if (!clientKey) {
        return NULL;
    }
    
    strcat(clientKey, GUID);
    sha1DataTemp = sha1_hash(clientKey);
    n = (int)strlen(sha1DataTemp);
    sha1Data = (char *)malloc(n / 2 + 1);
    memset(sha1Data, 0, n / 2 + 1);
    for (i = 0; i < n; i += 2) {
        sha1Data[i / 2] = htoi(sha1DataTemp, i, 2);
    }
    serverKey = base64_encode(sha1Data, (int)strlen(sha1Data));
    
    return serverKey;
}

int shakeHand(int fd) {
    char buff[DEFAULT_BUFF_SIZE];
    char responseHeader[DEFAULT_BUFF_SIZE];
    char *acceptKey;
    if (!fd) {
        return -1;
    }
    if(read(fd, buff, DEFAULT_BUFF_SIZE) < 0) {
        return -1;
    }
    acceptKey = getAcceptKey(buff);
    if (!acceptKey) {
        return -1;
    }
    
    memset(responseHeader, '\0', DEFAULT_BUFF_SIZE);
    sprintf(responseHeader, "HTTP/1.1 101 Switching Protocols\r\n");
    sprintf(responseHeader, "%sUpgrade: websocket\r\n", responseHeader);
    sprintf(responseHeader, "%sConnection: Upgrade\r\n", responseHeader);
    sprintf(responseHeader, "%sSec-WebSocket-Accept: %s\r\n\r\n", responseHeader, acceptKey);
    
    return (int)write(fd, responseHeader, strlen(responseHeader));
}

int packData(char* frame, char* data, int dataLen) {
    int len = 0;
    if (dataLen < 126) {
        memset(frame, 0, dataLen + 2);
        frame[0] = 0x81;
        frame[1] = dataLen;
        memcpy(frame + 2, data, dataLen);
        len = dataLen + 2;
    } else if (dataLen < 0xFFFF) {
        memset(frame, 0, dataLen + 4);
        frame[0] = 0x81;
        frame[1] = 0x7e;
        frame[2] = (dataLen >> 8 & 0xFF);
        frame[3] = (dataLen & 0xFF);
        memcpy(frame + 4, data, dataLen);
        len = dataLen + 4;
    } else {
        //有空实现
        len = 0;
    }
    
    return len;
}

int sendResponse(int fd, char* data, int dataLen) {
    int len = 0;
    char buff[DEFAULT_BUFF_SIZE + 4];
    if (fd < 0) {
        return len;
    }
    
    int frameLen = packData(buff, data, dataLen);
    if (frameLen <= 0) {
        return len;
    }
    
    return (int)write(fd, buff, frameLen);
}

void closeConn(int fd) {
    //shutdown(fd, 2);
    close(fd);
}

int connectToRemote(const char *hostname, const char *serv) {
    int sockfd, n;
    
    struct addrinfo hints, *res, *ressave;
    bzero(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if ((n = getaddrinfo(hostname, serv, &hints, &res)) != 0) {
        printf("tcp connect error for %s, %s:%s\n", hostname, serv, gai_strerror(n));
        return -1;
    }
    
    ressave = res;
    
    do {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0) continue;
        
        if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0) break;
        close(sockfd);
    } while ((res = res->ai_next) != NULL);
    
    if (res == NULL) {
        printf("tcp connect error for %s, %s\n", hostname, serv);
        return -1;
    }
    
    freeaddrinfo(ressave);
    return sockfd;
}

void pipeForRemote(int connfd, int remotefd) {
    int len = 0;
    char buff[DEFAULT_BUFF_SIZE];
    while ((len = (int)recv(remotefd, buff, sizeof(buff), 0)) > 0) {
        sendResponse(connfd, buff, len);
    }
}

void pipeForLocal(int connfd, int remotefd) {
    printf("current fd:%d in pipeForLocal\n", connfd);
    int len = 0;
    char buff[DEFAULT_BUFF_SIZE];
    while ((len = getRequest(connfd, buff)) > 0) {
        send(remotefd, buff, len, 0);
    }
}

void* __pipeForLocal(void* args) {
#ifdef __APPLE__
    pthread_setname_np("__pipeForLocal"); //调试用
#endif
    struct conn* conn = (struct conn*)args;
    printf("current fd:%d in __pipeForLocal\n", conn->localfd);
    pipeForLocal(conn->localfd, conn->remotefd);
    pthread_mutex_lock(&conn->finMutex);
    conn->fin = 1;
    pthread_cond_signal(&conn->finCond);
    pthread_mutex_unlock(&conn->finMutex);
    return (void*)0;
}

void* handleConnection(void *args) {
    char buff[DEFAULT_BUFF_SIZE];
    for (;;) {
        pthread_mutex_lock(&server.connMutex);
        while (server.currConnCount == 0) {
            pthread_cond_wait(&server.connEmptyCond, &server.connMutex);
        }
        
        server.currPoolIndex--;
        server.currConnCount--;
        struct conn conn = server.pool[server.currPoolIndex];
        pthread_cond_signal(&server.connFullCond);
        pthread_mutex_unlock(&server.connMutex);
        
        if(shakeHand(conn.localfd) <= 0) {
            closeConn(conn.localfd);
        }
        memset(buff, 0, sizeof(buff));
        int len = getRequest(conn.localfd, buff);
        if(len > 0) {
            char* header = buff+1;
            cJSON* json = cJSON_Parse(header);
            if(json) {
                cJSON *service = cJSON_GetObjectItem(json, "Service");
                // cJSON *type = cJSON_GetObjectItem(json, "Type");
                IString istr;
                if (Split(service->valuestring, ":", &istr)) {
                    conn.remotefd = connectToRemote(istr.str[0], istr.str[1]);
                    if (conn.remotefd < 0) {
                        printf("failed to connect remote server.\n");
                    } else {
                        pthread_t *tidptr = (pthread_t*)malloc(sizeof(pthread_t));
                        pthread_create(tidptr, NULL, __pipeForLocal, (void*)&conn);
                        pipeForRemote(conn.localfd, conn.remotefd);
                        
                        closeConn(conn.localfd);
                        closeConn(conn.remotefd);
                    }
                }
            }
        }
        
    close:
        closeConn(conn.localfd);
    }
    
    return 0;
}

void* __handleConnection(void *args) {
    char buff[DEFAULT_BUFF_SIZE];
#ifdef __APPLE__
    pthread_setname_np("handleConnection"); //调试用
#endif
    struct conn* conn = (struct conn*)args;
    conn->fin = 0;
    pthread_mutex_init(&conn->finMutex, NULL);
    pthread_cond_init(&conn->finCond, NULL);
    
    printf("current fd:%d in __handleConnection\n", conn->localfd);
    
    if(shakeHand(conn->localfd) <= 0) {
        closeConn(conn->localfd);
    }
    memset(buff, 0, sizeof(buff));
    int len = getRequest(conn->localfd, buff);
    if(len > 0) {
        char* header = buff+1;
        cJSON* json = cJSON_Parse(header);
        if(json) {
            cJSON *service = cJSON_GetObjectItem(json, "Service");
            // cJSON *type = cJSON_GetObjectItem(json, "Type");
            IString istr;
            if (Split(service->valuestring, ":", &istr)) {
                conn->remotefd = connectToRemote(istr.str[0], istr.str[1]);
                if (conn->remotefd < 0) {
                    printf("failed to connect remote server.\n");
                } else {
                    pthread_t tid;
                    pthread_create(&tid, NULL, __pipeForLocal, (void*)conn);
                    pipeForRemote(conn->localfd, conn->remotefd);
                    while (!conn->fin) {
                        pthread_cond_wait(&conn->finCond, &conn->finMutex);
                    }
                    closeConn(conn->localfd);
                    closeConn(conn->remotefd);
                }
            }
        }
    }
    
    return 0;
}

void waitSignal(int signal) {return;}

void runWithWebsocket(struct cuteServer *server) {
    int i;
    for (i = 0; i < server->poolSize; i++) {
        if (pthread_create(&server->tids[i], NULL, server->handleConnection, NULL) != 0)
            error("failed to create threads.\n");
    }
    
    for (;;) {
        pthread_mutex_lock(&server->connMutex);
        while (server->currConnCount > server->poolSize) {
            pthread_cond_wait(&server->connFullCond, &server->connMutex);
        }
        
        struct conn conn;
        socklen_t len = sizeof(len);
        memset(&conn.localAddr, 0, sizeof(conn.localAddr));
        printf("accepting...\n");
        conn.localfd = accept(server->fd, (struct sockaddr *)&conn.localAddr, &len);
        server->pool[server->currPoolIndex] = conn;
        server->currConnCount++;
        server->currPoolIndex++;
        pthread_cond_broadcast(&server->connEmptyCond);
        pthread_mutex_unlock(&server->connMutex);
    }
}
void __runWithWebsocket(struct cuteServer *server) {
    signal(SIGPIPE, SIG_IGN);
    int i;
    for (;;) {
        struct conn* conn = (struct conn*)malloc(sizeof(struct conn));
        socklen_t len = sizeof(len);
        memset(&conn->localAddr, 0, sizeof(conn->localAddr));
        conn->localfd = accept(server->fd, (struct sockaddr *)&conn->localAddr, &len);
        printf("accepting...\n");
        pthread_create(&server->tids[i], NULL, server->handleConnection, (void*)conn);
    }
}

void test() {
    connectToRemote("euphie.me", "80");
    sleep(20);
    exit(0);
}

//#define M_WORKER

void parseOptions(int argc, char *argv[]) {
    int c;
    memset(&server.addr, 0, sizeof(server.addr));
    server.addr.sin_family = AF_INET;
    server.addr.sin_port = htons(DEFAULT_PORT);
    server.addr.sin_addr.s_addr = htonl(INADDR_ANY);
    while ((c = getopt(argc, argv, "p:l:h")) != -1) {
        switch(c) {
            case 'p':
                server.addr.sin_port = htons(atoi(optarg));
                break;
            case 'l':
                server.addr.sin_addr.s_addr = inet_addr(optarg);
                break;
            case 'h':
                printf("usage: cute -l 127.0.0.1 -p 9999\n");
                exit(0);
                break;
        }
    }
    
    return;
}

int main(int argc, char *argv[]) {
     //test();
    parseOptions(argc, argv);
    server.poolSize = 20;
    server.currConnCount = 0;
    server.maxConnCount = 100;
    server.currPoolIndex = 0;
    server.pid = getpid();
    server.handleSignal = waitSignal;
#ifdef M_WORKER
    server.runServer = runWithWebsocket;
    server.handleConnection = handleConnection;
#else
    server.runServer = __runWithWebsocket;
    server.handleConnection = __handleConnection;
#endif
    server.tids = (pthread_t *)malloc(sizeof(pthread_t) * server.poolSize);
    server.pool = (struct conn *)malloc(sizeof(struct conn) * server.poolSize);
    pthread_mutex_init(&server.connMutex, NULL);
    pthread_cond_init(&server.connEmptyCond, NULL);
    pthread_cond_init(&server.connFullCond, NULL);
    
    server.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server.fd < 0) {
        error("failed to create socket.\n");
    }
    
    if (bind(server.fd, (struct sockaddr *)&server.addr, sizeof(server.addr)) < 0) {
        error("failed to bind the address.\n");
    }
    
    if (listen(server.fd, server.maxConnCount * 2) < 0) {
        error("failed to listen.\n");
    }
    printf("server %s:%d has started...\n", inet_ntoa(server.addr.sin_addr), ntohs(server.addr.sin_port));
    server.runServer(&server);
}