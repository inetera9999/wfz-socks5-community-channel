#ifndef __SOCKS5_H__
#define __SOCKS5_H__ 

#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <assert.h>

#include "buff.h"

#if (EAGAIN != EWOULDBLOCK)
	#define EAGAIN_EWOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
	#define EAGAIN_EWOULDBLOCK EAGAIN
#endif

#define LOG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__); fprintf(stderr, "\n")

typedef struct sockaddr sockaddr_t;
typedef struct sockaddr_in sockaddr_in_t;
typedef struct sockaddr_in6 sockaddr_in6_t;
typedef struct addrinfo addrinfo_t;
typedef struct epoll_event epoll_event_t;

typedef void read_cb(int fd, void *ud);
typedef void write_cb(int fd, void *ud);

#define INIT_BUFF_CAP 1024

#define MAX_UNAME_LEN 20
#define MAX_PASSWD_LEN 20
#define BLACKLOG 4096
#define MAX_EPOLL_EVENTS 64

typedef enum sock_keeplive_T { // 当前连接是否保持
    conn_keep_alive_noclear = 0,  // 未指定
	conn_keep_alive,
	conn_keep_close // 访问后关闭
} sock_keeplive_t;

typedef enum tunnel_state {
	open_state,
	auth_state,
	request_state,
	connecting_state, // connecting to remote
	connected_state,  // connected to remote
	tunnel_waitting // by wfz, 远端等待状态
} tunnel_state_t;

typedef enum tunnel_status_T { // 隧道状态，-1：尚未建立 0：自由，1：正在占用，2：待回收或重启
    tunn_is_invalid = -1,  // 显式指定为 -1，尚未建立
    tunn_is_idle,       // 默认为前一个值 +1，即 0 -- 空闲状态
    tunn_is_run,   // 占用状态
	tunn_is_free, //可回收或重启
	tmptun_is_wait,
	tmptun_is_alive, //连接被保持
	// tmptun_is_run,
	tmptun_is_free,
	tmptun_is_close
} tunnel_status_t;

typedef struct open_protocol {
	uint8_t ver;
	uint8_t nmethods;
	uint8_t methods[255];
} open_protocol_t;

typedef struct auth_protocol {
	uint8_t ver;
	uint8_t ulen;
	char uname[255];
	uint8_t plen;
	char passwd[255];
} auth_protocol_t;

typedef struct request_protocol {
	uint8_t ver;
	uint8_t cmd;
	uint8_t rsv;
	uint8_t atyp;
	uint8_t domainlen;
	char addr[255];
	uint16_t port;
} request_protocol_t;

typedef struct sock sock_t;
typedef struct tunnel {
	// sock_t *client_sock;
	sock_t *priv_sock; // private socket
	sock_t *remote_sock;

	tunnel_state_t state;
	open_protocol_t op;
	auth_protocol_t ap;
	request_protocol_t rp;
	size_t read_count;
	tunnel_status_t status; //tunn状态，-1：尚未建立 0：自由，1：正在占用，2：待回收或重启
	int closed;
} tunnel_t;

typedef enum sock_state {
	sock_connecting,
	sock_connected,
	sock_halfclosed,
	sock_closed,
} sock_state_t;

struct sock {
	int fd;
	read_cb *read_handle;
	write_cb *write_handle;
	buff_t *read_buff;
	buff_t *write_buff;
	tunnel_t *tunnel;
	sock_state_t state;
	int ispriv; //private host -- 自有主机	
	int8_t iskeepalive; //sock_keeplive_t
	// int isclient;
};

typedef struct server {
	int listenfd;
	// int rmtlisnfd;
	// read_cb *read_handle;
	// int epollfd;
	// char username[255];
	// char passwd[255];
} server_t;

typedef struct enent_monitor_T {
	// int listenfd;
	// int rmtlisnfd;
	// read_cb *read_handle;
	int epollfd;
	char username[255];
	char passwd[255];
} evntmon_t;

typedef struct mediator_T {
	int mediafd;
	int localfd;
	// read_cb *read_handle;
	int epollfd;
	char username[255];
	char passwd[255];
} mediator_t;

#endif