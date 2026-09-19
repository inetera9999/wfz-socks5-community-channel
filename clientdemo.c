#define _POSIX_C_SOURCE 200112L

#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <assert.h>

#include "util.h"

mediator_t MEDIA;
static char g_locAddr[128];
static char g_locPort[16];

static tunnel_t *tunnel_create(int fd, int privflg);
static void tunnel_release(tunnel_t *tunnel);
static void tunnel_shutdown(tunnel_t *tunnel);
static void tunnel_read_handle(int fd, void *ud);
static void tunnel_write_handle(int fd, void *ud);
static int tunnel_open_handle(tunnel_t *tunnel);
static int tunnel_auth_handle(tunnel_t *tunnel);
static int tunnel_request_handle(tunnel_t *tunnel);
static int tunnel_connecting_handle(tunnel_t *tunnel);
static int tunnel_connected_handle(tunnel_t *tunnel, int ispriv);
static int tunnel_write_client(tunnel_t *tunnel, void *src, size_t size);
static int medSvr_init(char *host, char *port, char *rmtPort, char *username, char *passwd);
static int openConnSock(char *host, char *port, int *sockFd);
int appCompare(connList_t *p1, connList_t *p2);
int waitCompare(connList_t *p1, connList_t *p2);
int waitCompare2(connList_t *p1, connList_t *p2);
connList_t *getWaitobjFrList(int rmtfd);
connList_t *getTargetFrList(int rmtfd, int privflg);
connList_t * waitLinePairing();
static int tunnel_write_request(tunnel_t *tunnel);
static int tunnel_connect_to_remote(tunnel_t *tunnel,char *addr, char *pPort);

static int epoll_add(sock_t *sock)
{
	epoll_event_t event;
	event.events = EPOLLIN;
	event.data.ptr = sock;
	return epoll_ctl(MEDIA.epollfd, EPOLL_CTL_ADD, sock->fd, &event);
}

static int epoll_del(sock_t *sock)
{
	epoll_event_t event;
	return epoll_ctl(MEDIA.epollfd, EPOLL_CTL_DEL, sock->fd, &event);
}

static int epoll_modify(sock_t *sock, int writable, int readable)
{
	epoll_event_t event;
	event.data.ptr = sock;
	event.events = (writable ? EPOLLOUT : 0) | (readable ? EPOLLIN : 0);
	return epoll_ctl(MEDIA.epollfd, EPOLL_CTL_MOD, sock->fd, &event);
}

static sock_t *sock_create(int fd, sock_state_t state, int ispriv, tunnel_t *tunnel)
{
	// todo: isclient  -- to reconfigure it
	sock_t *sock = (sock_t *)malloc(sizeof(*sock));
	if (sock == NULL)
		return NULL;
	memset(sock, 0, sizeof(*sock));

	buff_t *read_buff = buff_create(INIT_BUFF_CAP);
	if (read_buff == NULL)
		return NULL;

	buff_t *write_buff = buff_create(INIT_BUFF_CAP);
	if (write_buff == NULL)
	{
		buff_release(read_buff);
		free(sock);
		return NULL;
	}

	sock->read_buff = read_buff;
	sock->write_buff = write_buff;
	sock->tunnel = tunnel;
	sock->fd = fd;
	sock->read_handle = tunnel_read_handle;
	sock->write_handle = tunnel_write_handle;
	sock->state = state;
	sock->ispriv = ispriv;
	return sock;
}

static void sock_release(sock_t *sock)
{
	tunnel_t *tunnel = sock->tunnel;
	printf("state[%d] status[%d] priv[%d] fd[%d] will close.\n", tunnel->state, 
			tunnel->status, sock->ispriv, sock->fd);

	buff_release(sock->write_buff);
	buff_release(sock->read_buff);

	if (sock->ispriv)
		tunnel->priv_sock = NULL;
	else
		tunnel->remote_sock = NULL;
	epoll_del(sock);
	close(sock->fd);
	free(sock);

	// when both client and remote sock release
	// release tunnel
	// if (tunnel->remote_sock == NULL && tunnel->priv_sock == NULL)
	// {
	// 	tunnel_release(tunnel);
	// }
}

/*
 * Receive rst or no more data to send or invalid peer, we should release sock
 * */
static void sock_force_shutdown(sock_t *sock)
{
	sock_release(sock);
}

/*
 * Receive fin, do not receive again,
 * If Append read_buff to other write_buff,
 * If write_buff not empty, we shoould still send data,
 * Otherwise force shutdown
 * */
static void sock_shutdown(sock_t *sock)
{
	sock->state = sock_halfclosed;

	tunnel_t *tunnel = sock->tunnel;
	// forward left data
	if (tunnel->state == connected_state)
	{
		if (sock->ispriv && tunnel->remote_sock != NULL)
			buff_concat(tunnel->remote_sock->write_buff, sock->read_buff);
		else if (tunnel->priv_sock != NULL)
			buff_concat(tunnel->priv_sock->write_buff, sock->read_buff);
	}

	int writable = buff_readable(sock->write_buff) > 0;
	if (writable)
		epoll_modify(sock, writable, 0);
	else {
		int n = 0;
		if ( sock == tunnel->remote_sock )
			n = 1;
		else if ( sock == tunnel->priv_sock )
			n = 2;
		sock_force_shutdown(sock);
		if (tunnel->state == tunnel_waitting) //fz: fd for real server
		{
			tunnel->status = tmptun_is_free;
			tunnel->remote_sock = NULL;
		}
		else if (tunnel->state == connected_state)
		{
			switch (n)
			{
			case 1:
				tunnel->status = tunn_is_idle;
				tunnel->remote_sock = NULL;
				break;
			case 2:
				tunnel->status = tunn_is_free;
				tunnel->priv_sock = NULL;
				break;
			
			default:
				break;
			}
		}
	}
}

static int sock_nonblocking(int fd)
{
	int flag;
	if ((flag = fcntl(fd, F_GETFL, 0)) < 0)
		return -1;
	if ((flag = fcntl(fd, F_SETFL, flag | O_NONBLOCK)) < 0)
		return -1;
	return flag;
}

static int sock_keepalive(int fd)
{
	int keepalive = 1;
	return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
}

static tunnel_t *tunnel_create(int fd, int privflg)
{
	volatile tunnel_t *tunnel;


	connList_t *pconn1 = getTargetFrList(fd,privflg);
	if (pconn1 == NULL) {
		close(fd);
		return NULL;
	}
	tunnel = pconn1->tunnel;

	sock_t *new_sock = sock_create(fd, sock_connected, privflg, tunnel);
	if (new_sock == NULL)
	{
		free(tunnel);
		close(fd);
		return NULL;
	}

	if (privflg)
	{
		tunnel->state = open_state;
		tunnel->priv_sock = new_sock;
		tunnel->read_count = 0;
		tunnel->closed = 0;

		// connList_t *pconn;
		// pconn = (connList_t *)malloc(sizeof(connList_t));
		// if (pconn == NULL)
		// {
		// 	printf("malloc error\n");
		// 	free(tunnel);
		// 	close(fd);
		// 	return NULL;
		// }
		// pconn->tunnel = tunnel;
		// AppListAdd(pconn); 

		epoll_add(new_sock);
		epoll_modify(new_sock, 1, 1);
	}
	else
	{
		// tunnel->remote_sock = new_sock; // to real-host

		// // tunnel->state = connected_state; //todo: refix here  by wfz
		// new_sock->state = sock_connected;

		// epoll_add(new_sock);
		// epoll_modify(new_sock, 1, 1);
		printf("Now it donot run to here: fd = %d\n", new_sock->fd);
	}

	return tunnel;
}

static void tunnel_shutdown(tunnel_t *tunnel)
{
	if (tunnel->priv_sock != NULL)
		sock_shutdown(tunnel->priv_sock);
	if (tunnel->remote_sock != NULL)
		sock_shutdown(tunnel->remote_sock);
}

void  tunnel_switch_handle(sock_t *sock, tunnel_t *tunnel) {
	sock_t *remote_sock = tunnel->remote_sock;
	// sock->state = sock_halfclosed;

	// tunnel_t *tunnel = sock->tunnel;
	// forward left data
	// if (tunnel->state == connected_state)
	// {
	// 	if (sock->ispriv && tunnel->remote_sock != NULL) //  by wfz
	// 		buff_concat(tunnel->remote_sock->write_buff, sock->read_buff);

	// 	else if (tunnel->priv_sock != NULL)
	// 		buff_concat(tunnel->priv_sock->write_buff, sock->read_buff);
	// }
	// else if (tunnel->status == tmptun_is_close )
	// { // by wfz
	// 	sock_force_shutdown(sock);
	// 	return;
	// }
	//写操作时，该函数会N次的调用，故这里不能处理 -- wfz
	if ( sock != tunnel->priv_sock )
		return;
	if ( remote_sock == NULL )
		return;

	int writable = buff_readable(remote_sock->read_buff) ;
	if (writable) {
		tunnel_connected_handle(tunnel, 0); //将后续读的数据写入到远程socket中
	}
	else {
		// printf("switch:tun-state:%d priv:%d keepalive=%d \n", tunnel->state,sock->ispriv, sock->iskeepalive);
		// if (sock->iskeepalive == conn_keep_alive) {

		// 	epoll_modify(sock, 0, 1);  // turn to read
		// 	// printf("remote-sock write end, enter read.\n");
		// 	// printf("do nothing.\n");
		// 	// printf(",");
		// } else {
		// 	epoll_modify(sock, 0, 1);  // turn to read
		// }
		
		if ( tunnel->status == tunn_is_run ) { // by wfz
			connList_t *conn1 = getWaitobjFrList(sock->fd);
			conn1->tunnel->remote_sock = tunnel->remote_sock;
			conn1->tunnel->remote_sock->tunnel = conn1->tunnel;
			conn1->tunnel->state = tunnel_waitting;
			conn1->tunnel->status = tmptun_is_alive;

			tunnel->status = tunn_is_idle;
			tunnel->remote_sock = NULL;
			epoll_modify(sock, 0, 1);
			printf(" ---> tunnel fd[%d] is free.\n", tunnel->priv_sock->fd);
		}
			
		
	}
}

static void tunnel_release(tunnel_t *tunnel)
{
	// free(tunnel); // by wfz
}

static void tunnel_read_handle(int fd, void *ud)
{
	sock_t *sock = (sock_t *)ud;
	tunnel_t *tunnel = sock->tunnel;

	int n = buff_readfd(sock->read_buff, fd);
	if (n < 0)
	{
		switch (errno)
		{
		case EINTR:
		case EAGAIN_EWOULDBLOCK:
			break;
		default:
			goto shutdown;
		}
	}
	else if (n == 0)
		goto shutdown;

	switch (tunnel->state)
	{
	case open_state:
		if (tunnel_open_handle(tunnel) < 0)
			goto force_shutdown;
		break;
	case auth_state:
		if (tunnel_auth_handle(tunnel) < 0)
			goto force_shutdown;
		break;
	case request_state:
		if (tunnel_request_handle(tunnel) < 0)
			goto force_shutdown;
		break;
	case connecting_state:
		assert(sock->ispriv == 0);
		if (tunnel_connecting_handle(tunnel) < 0)
			goto tunnel_shutdown;
		break;
	case connected_state:
		// if (tunnel_connected_handle(tunnel, sock->ispriv) < 0)
		// 	goto tunnel_shutdown;
		tunnel_connected_handle(tunnel, sock->ispriv);
		break;
	default:
		// assert(0); //note: what happend
		printf("what happend:tun-state[%d] status[%d] priv[%d] fd[%d] will close.\n", tunnel->state, 
			tunnel->status, sock->ispriv, sock->fd);
		printf("what happend[%d] [%.*s]\n",sock->fd, 
			sock->read_buff->write_index - sock->read_buff->read_index,
			sock->read_buff->data+sock->read_buff->read_index ); //todo: to be comment
		break;
	}
	return;

force_shutdown: // peer invalid
	sock_force_shutdown(sock);
	return;

shutdown: // half closed
	sock_shutdown(sock);
	return;

tunnel_shutdown: // half closed both client and remote
	tunnel_shutdown(tunnel);
}

static void tunnel_write_handle(int fd, void *ud)
{
	sock_t *sock = (sock_t *)ud;
	tunnel_t *tunnel = sock->tunnel;

	if (buff_readable(sock->write_buff) > 0)
	{
		int n = buff_writefd(sock->write_buff, fd);
		if (n <= 0)
		{
			switch (errno)
			{
			case EINTR:
			case EAGAIN_EWOULDBLOCK:
				break;
			default:
				goto force_shutdown;
			}
		}
	}
	else if (sock->state == sock_halfclosed)
	{
		goto force_shutdown;
	}

	if (tunnel->state == connecting_state)
	{
		assert(sock->ispriv == 0);

		if (tunnel_connecting_handle(tunnel) < 0)
			goto tunnel_shutdown;
	}


	int writable = buff_readable(sock->write_buff) > 0;
	epoll_modify(sock, writable, 1);

	if (tunnel->status == tunn_is_run)
	{
		goto tunnel_switch;
	}
	return;

tunnel_shutdown:
	tunnel_shutdown(tunnel);
	return;

force_shutdown:
	sock_force_shutdown(sock);
	return;

tunnel_switch:
	tunnel_switch_handle(sock, tunnel);
	return;	
}

// |VER(1)|NMETHODS(1)|METHODS(1-255)|
static int tunnel_open_handle(tunnel_t *tunnel)
{
	buff_t *buff = tunnel->priv_sock->read_buff;
	open_protocol_t *op = &tunnel->op;
	size_t *nreaded = &tunnel->read_count;
	size_t nheader = sizeof(op->ver) + sizeof(op->nmethods);

	if (*nreaded == 0)
		goto header;
	// else if(*nreaded == nheader) goto methods;
	else
		assert(0);

header:
	// VER(1)|NMETHODS(1)
	if (buff_readable(buff) >= nheader)
	{
		printf("fd=%d recv version-rsp..\n",  tunnel->priv_sock->fd);
		buff_read(buff, &op->ver, sizeof(op->ver));
		if (op->ver != 0x05)
			return -1;

		buff_read(buff, &op->nmethods, sizeof(op->nmethods)); // METHOD
		*nreaded += nheader;

		// todo : send request
		tunnel_write_request(tunnel);
		tunnel->state = request_state;
		epoll_modify(tunnel->priv_sock, 1, 1);

		*nreaded = 0;
	}
	else
		return 0;

	// methods:
	// 	// METHODS(1-255)
	// 	if (buff_readable(buff) >= op->nmethods) {
	// 		buff_read(buff, op->methods, op->nmethods);

	// 		uint8_t reply[2];
	// 		reply[0] = 0x05; // socks5
	// 		int auth = strcmp(MEDIA.username, "") != 0 && strcmp(MEDIA.passwd, "");
	// 		if (auth) {
	// 			reply[1] = 0x02;
	// 			tunnel->state = auth_state;
	// 		} else {
	// 			reply[1] = 0x00;
	// 			tunnel->state = request_state;
	// 		}
	// 		*nreaded = 0;
	// 		return tunnel_write_client(tunnel, reply, sizeof(reply));
	// 	} else return 0;

	return 0;
}

// |VER(1)|ULEN(1)|UNAME(1-255)|PLEN(1)|PASSWD(1-255)|
static int tunnel_auth_handle(tunnel_t *tunnel)
{
	buff_t *buff = tunnel->priv_sock->read_buff;
	auth_protocol_t *ap = &tunnel->ap;
	size_t *nreaded = &tunnel->read_count;
	size_t nheader = sizeof(ap->ver) + sizeof(ap->ulen);
	size_t nplen = sizeof(ap->plen);

	if (*nreaded == 0)
		goto header;
	else if (*nreaded == nheader)
		goto uname;
	else if (*nreaded == nheader + ap->ulen)
		goto plen;
	else if (*nreaded == nheader + ap->ulen + nplen)
		goto passwd;
	else
		assert(0);

header:
	// VER(1)|ULEN(1)
	if (buff_readable(buff) >= nheader)
	{
		buff_read(buff, &ap->ver, sizeof(ap->ver));
		buff_read(buff, &ap->ulen, sizeof(ap->ulen));
		if (ap->ulen > MAX_UNAME_LEN)
			return -1;

		*nreaded += nheader;
	}
	else
		return 0;

uname:
	// UNAME(1-255)
	if (buff_readable(buff) >= ap->ulen)
	{
		buff_read(buff, ap->uname, ap->ulen);
		*nreaded += ap->ulen;
	}
	else
		return 0;

plen:
	// PLEN(1)
	if (buff_readable(buff) >= nplen)
	{
		buff_read(buff, &ap->plen, nplen);
		if (ap->plen > MAX_PASSWD_LEN)
			return -1;
		*nreaded += nplen;
	}
	else
		return 0;

passwd:
	// PASSWD(1-255)
	if (buff_readable(buff) >= ap->plen)
	{
		buff_read(buff, ap->passwd, ap->plen);
		if (strcmp(ap->uname, MEDIA.username) != 0 || strcmp(ap->passwd, MEDIA.passwd) != 0)
			return -1;

		uint8_t reply[2];
		reply[0] = ap->ver; // subversion
		reply[1] = 0x00;	// success

		if (tunnel_write_client(tunnel, reply, sizeof(reply)) < 0)
			return -1;

		tunnel->state = request_state;
		*nreaded = 0;
	}
	else
		return 0;

	return 0;
}

static int tunnel_write_request(tunnel_t *tunnel)
{
	// sockaddr_t sa;
	// socklen_t len = sizeof(sa);
	// uint8_t header[4];
	// header[0] = 0x05; // socks5
	// header[1] = 0x01; // CONNECT
	// header[2] = 0x00;

	// //非标准 -- by wfz
	// header[3] = 0x03; //DOMAINNAME

	request_protocol_t rp; // socks5
	rp.ver = 0x05;
	rp.cmd = 0x01; // CONNECT
	rp.rsv = 0x00;
	rp.atyp = 0x03; // DOMAINNAME

	sprintf(rp.addr, "%s", "www.hellofz.org");
	rp.domainlen = strlen(rp.addr);
	// if (tunnel_write_client(tunnel, &rp, 5) < 0) return -1;
	int len = 5 + rp.domainlen;
	if (tunnel_write_client(tunnel, &rp, len) < 0)
		return -1;
	rp.port = htons(8090);
	if (tunnel_write_client(tunnel, &rp.port, sizeof(rp.port)) < 0)
		return -1;
	printf("fd=%d send request readying..\n", tunnel->priv_sock->fd);
	// by wfz
	// if (getsockname(tunnel->remote_sock->fd, &sa, &len) < 0) return -1;

	// if (sa.sa_family == AF_INET) {
	// 	header[3] = 0x01; //IPV4
	// 	if (tunnel_write_client(tunnel, header, sizeof(header)) < 0) return -1;

	// 	sockaddr_in_t *sa_in = (sockaddr_in_t*)&sa;
	// 	if (tunnel_write_client(tunnel, &sa_in->sin_addr, sizeof(sa_in->sin_addr)) < 0) return -1;
	// 	if (tunnel_write_client(tunnel, &sa_in->sin_port, sizeof(sa_in->sin_port)) < 0) return -1;
	// } else if (sa.sa_family == AF_INET6) {
	// 	header[3] = 0x04; //IPV6
	// 	tunnel_write_client(tunnel, header, sizeof(header));

	// 	sockaddr_in6_t *sa_in6 = (sockaddr_in6_t*)&sa;
	// 	tunnel_write_client(tunnel, &sa_in6->sin6_addr, sizeof(sa_in6->sin6_addr));
	// 	tunnel_write_client(tunnel, &sa_in6->sin6_port, sizeof(sa_in6->sin6_port));
	// } else {
	// 	LOG("tunnel_notify_connected,unexpected family=%d", sa.sa_family);
	// 	return -1;
	// }

	return 0;
}

// |VER(1)|REP(1)|RSV(1)|ATYP(1))|BIND.ADDR(variable)|BIND.PORT(2)|
static int tunnel_notify_connected(tunnel_t *tunnel)
{
	sockaddr_t sa;
	socklen_t len = sizeof(sa);
	uint8_t header[4];
	header[0] = 0x05; // socks5
	header[1] = 0x00; // success
	header[2] = 0x00;

	// 非标准 -- by wfz
	header[3] = 0x00; // none
	if (tunnel_write_client(tunnel, header, sizeof(header)) < 0)
		return -1;

	// by wfz
	// if (getsockname(tunnel->remote_sock->fd, &sa, &len) < 0) return -1;

	// if (sa.sa_family == AF_INET) {
	// 	header[3] = 0x01; //IPV4
	// 	if (tunnel_write_client(tunnel, header, sizeof(header)) < 0) return -1;

	// 	sockaddr_in_t *sa_in = (sockaddr_in_t*)&sa;
	// 	if (tunnel_write_client(tunnel, &sa_in->sin_addr, sizeof(sa_in->sin_addr)) < 0) return -1;
	// 	if (tunnel_write_client(tunnel, &sa_in->sin_port, sizeof(sa_in->sin_port)) < 0) return -1;
	// } else if (sa.sa_family == AF_INET6) {
	// 	header[3] = 0x04; //IPV6
	// 	tunnel_write_client(tunnel, header, sizeof(header));

	// 	sockaddr_in6_t *sa_in6 = (sockaddr_in6_t*)&sa;
	// 	tunnel_write_client(tunnel, &sa_in6->sin6_addr, sizeof(sa_in6->sin6_addr));
	// 	tunnel_write_client(tunnel, &sa_in6->sin6_port, sizeof(sa_in6->sin6_port));
	// } else {
	// 	LOG("tunnel_notify_connected,unexpected family=%d", sa.sa_family);
	// 	return -1;
	// }

	return 0;
}

// |VER(1)|CMD(1))|RSV(1)|ATYP(1)|DST.ADDR(variable)|DST.PORT(2)|
#define NIPV4 4
#define NIPV6 16

static int tunnel_request_handle(tunnel_t *tunnel)
{
	buff_t *buff = tunnel->priv_sock->read_buff;
	request_protocol_t *rp = &tunnel->rp;
	size_t *nreaded = &tunnel->read_count;
	size_t nheader = sizeof(rp->ver) + sizeof(rp->cmd) + sizeof(rp->rsv) + sizeof(rp->atyp);
	size_t ndomainlen = sizeof(rp->domainlen);
	size_t nport = sizeof(rp->port);

	printf("tunnel_request_rsp: fd=%d, len=%ld\n", tunnel->priv_sock->fd, *nreaded);
	if (*nreaded == 0)
		goto header;
	else if (*nreaded == nheader)
		goto addr;
	else if (*nreaded == nheader + ndomainlen)
		goto domain;
	else
		assert(0);

header:
	// VER(1)|CMD(1))|RSV(1)|ATYP(1)
	if (buff_readable(buff) >= nheader)
	{
		buff_read(buff, &rp->ver, sizeof(rp->ver));
		if (rp->ver != 0x05)
			return -1;

		buff_read(buff, &rp->cmd, sizeof(rp->cmd));
		switch (rp->cmd)
		{
		case 0x01: // CONNECT
			printf("tunnel_request_rsp: fd=%d, cmd=%d\n", tunnel->priv_sock->fd, rp->cmd);
			break;
		case 0x02: // TODO implement BIND
		case 0x03: // TODO implement ASSOCIATE
		default:
			LOG("tunnel_request_handle,CMD not support,cmd=%d", rp->cmd);
			return -1;
		}

		buff_read(buff, &rp->rsv, sizeof(rp->rsv));
		buff_read(buff, &rp->atyp, sizeof(rp->atyp));
		*nreaded += nheader;
	}
	else
		return 0;

addr:
	printf("tunnel_request_rsp: fd=%d, enter addr\n", tunnel->priv_sock->fd);
	switch (rp->atyp)
	{
	case 0x01: // IPV4
		// DST.ADDR(variable)|DST.PORT(2)
		if (buff_readable(buff) >= NIPV4 + nport)
		{
			buff_read(buff, rp->addr, NIPV4);
			buff_read(buff, &rp->port, nport);
			printf("fz-ipv4 addr:%s  port:%d\n", rp->addr, ntohs(rp->port));
			tunnel->state = connected_state; // by wfz
		}
		else
			return 0;
		break;
	case 0x04: // IPV6
		// DST.ADDR(variable)|DST.PORT(2)
		if (buff_readable(buff) >= NIPV6 + nport)
		{
			buff_read(buff, rp->addr, NIPV6);
			buff_read(buff, &rp->port, nport);
			printf("fz-ipv6 addr:%s port:%d\n", rp->addr, ntohs(rp->port));
			tunnel->state = connected_state; // by wfz
		}
		else
			return 0;
		break;
	case 0x03: // DOMAIN
	{
		// DST.ADDR[0](1)
		if (buff_readable(buff) >= ndomainlen)
		{
			buff_read(buff, &rp->domainlen, ndomainlen);
			*nreaded += ndomainlen;
		}
		else
			return 0;

	domain:
		printf("tunnel_request_rsp: fd=%d, enter domain\n", tunnel->priv_sock->fd);
		// DST.ADDR[1](DST.ADDR[0])|DST.PORT(2)
		if (buff_readable(buff) >= rp->domainlen + nport)
		{
			buff_read(buff, rp->addr, rp->domainlen);
			buff_read(buff, &rp->port, nport);
			printf("fz-domain name:%s port:%d\n", rp->addr, ntohs(rp->port));
			tunnel->state = connected_state; // by wfz
		}
		else
			return 0;
	}
	break;
	default:
		return -1;
	}

	*nreaded = 0;
	// return tunnel_connect_to_remote(tunnel); //by fz
	// tunnel_notify_connected(tunnel); //todo: refix here!
	return 0;
}

static int tunnel_connecting_handle(tunnel_t *tunnel)
{
	int error;
	socklen_t len = sizeof(error);
	int code = getsockopt(tunnel->remote_sock->fd, SOL_SOCKET, SO_ERROR, &error, &len);
	/*
	 * If error occur, Solairs return -1 and set error to errno.
	 * Berkeley return 0 but not set errno.
	 */
	if (code < 0 || error)
	{
		if (error)
			errno = error;
		return -1;
	}

	tunnel->state = connected_state;
	tunnel->remote_sock->state = sock_connected;
	return tunnel_notify_connected(tunnel);
}

/*
 * For data forward:
 * If client readable, append client_read_buff to remote_write_buff
 * Else, append remote_read_buff to client_write_buff
 */
static int tunnel_connected_handle(tunnel_t *tunnel, int isprivate)
{
	if (isprivate)
	{
		if (tunnel->remote_sock == NULL) {
			int i;
			connList_t * conn1;
			conn1 = waitLinePairing();
			if (conn1 != NULL) {
				printf("++ get alive fd %d\n", conn1->tunnel->remote_sock->fd);
				conn1->tunnel->status = tmptun_is_free;
				tunnel->remote_sock = conn1->tunnel->remote_sock;
				tunnel->status = tunn_is_run;
				conn1->tunnel->remote_sock = NULL;
			}
			else {
				i =  tunnel_connect_to_remote(tunnel,g_locAddr, g_locPort);
				if (i < 0) {
					//todo: error proc
					return -1;
				}
			}
		}

		if ( buff_readable(tunnel->priv_sock->read_buff)>0 /*&& sock == tunnel->priv_sock*/ ) {
			printf("req to locfd[%d] [%.*s]\n",tunnel->remote_sock->fd, 
				tunnel->priv_sock->read_buff->write_index - tunnel->priv_sock->read_buff->read_index,
				tunnel->priv_sock->read_buff->data+tunnel->priv_sock->read_buff->read_index ); //todo: to be comment
			if (buff_concat(tunnel->remote_sock->write_buff, tunnel->priv_sock->read_buff) < 0)
				return -1;
			buff_clear(tunnel->priv_sock->read_buff);
			epoll_modify(tunnel->remote_sock, 1, 1);
		}
	}
	else
	{
		if (tunnel->priv_sock == NULL)
			return -1;
		if ( buff_readable(tunnel->remote_sock->read_buff)>0 /*&& sock == tunnel->remote_sock*/ ) {
			printf("response to tunfd[%d] [%.*s]\n", tunnel->priv_sock->fd,
				tunnel->remote_sock->read_buff->write_index - tunnel->remote_sock->read_buff->read_index,
				tunnel->remote_sock->read_buff->data+tunnel->remote_sock->read_buff->read_index ); //todo: to be comment
			if (buff_concat(tunnel->priv_sock->write_buff, tunnel->remote_sock->read_buff) < 0)
				return -1;
			buff_clear(tunnel->remote_sock->read_buff);
			epoll_modify(tunnel->priv_sock, 1, 1);
		}
	}
	return 0;
}

static int tunnel_write_client(tunnel_t *tunnel, void *src, size_t size)
{
	if (tunnel->priv_sock == NULL)
		return -1;

	if (buff_write(tunnel->priv_sock->write_buff, src, size) < 0)
		return -1;

	epoll_modify(tunnel->priv_sock, 1, 1);
	return 0;
}

// static void accept_handle(int privflg)
// {
// 	int newfd;
// 	if ((newfd = accept(MEDIA.mediafd, NULL, NULL)) < 0) {
// 		LOG("accept_handle failed,mediafd=%d,err=%s", MEDIA.mediafd, strerror(errno));
// 		return;
// 	}

// 	tunnel_create(newfd,privflg);
// }

static void sigign()
{
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
}

static int media_start()
{
	epoll_event_t events[MAX_EPOLL_EVENTS];
	for (;;)
	{
		int n = epoll_wait(MEDIA.epollfd, events, MAX_EPOLL_EVENTS, -1);
		if (n < 0 && errno != EINTR)
		{
			LOG("epoll_wait failed,error=%s", strerror(errno));
			return -1;
		}

		for (int i = 0; i < n; ++i)
		{
			void *cur_ud = events[i].data.ptr;
			int cur_fd = *(int *)cur_ud;
			int cur_events = events[i].events;
			if (cur_events & EPOLLIN)
			{
				// if (cur_fd == MEDIA.mediafd) {
				// 	accept_handle(1);
				// }
				// else if (cur_fd == MEDIA.localfd) {
				// 	accept_handle(0);
				// }
				// else
				{
					tunnel_read_handle(cur_fd, cur_ud);
				}
			}
			else if (cur_events & EPOLLOUT)
			{
				tunnel_write_handle(cur_fd, cur_ud);
			}
			else
			{
				LOG("unexpected epoll events");
			}
		}
	}

	return 0;
}

static int medSvr_init(char *host, char *port, char *rmtPort, char *username, char *passwd)
{

	int mediafd; // , localfd;
	if (openConnSock(host, port, &mediafd) < 0)
	{
		LOG("openConnSock failed, errno=%s", strerror(errno));
		return -1;
	}

	// if (openConnSock(host, rmtPort, &localfd) < 0)
	// {
	// 	LOG("openConnSock failed, errno=%s", strerror(errno));
	// 	return -1;
	// }

	int epollfd = -1;
	// 	原始版本：epoll_create(int size)
	// int epfd = epoll_create(10);  // size 参数已过时（仅作提示）
	// 参数 size 在 Linux 2.6.8 后不再有意义（内核动态调整），但必须 > 0
	// 改进版本：epoll_create1(int flags) (Linux 2.6.27+)
	// EPOLL_CLOEXEC 标志：设置文件描述符的 close-on-exec 属性:安全优势：防止子进程意外继承 epoll 实例
	//                                                       避免竞态条件：创建和设置原子操作
	// epoll_create1 的主要优势在于安全性和现代特性支持
	// if ((epollfd = epoll_create(1024)) < 0) {
	// 	LOG("epoll_create, errno=%s", strerror(errno));
	// 	return -1;
	// }
	// 
	// 	注：兼容旧系统 (Linux < 2.6.27):
	// int epfd = epoll_create(1);
	// if (epfd >= 0) {
	//     // 手动设置 close-on-exec
	//     fcntl(epfd, F_SETFD, FD_CLOEXEC);
	// }
	if ((epollfd = epoll_create1(EPOLL_CLOEXEC)) < 0) {
		LOG("epoll_create1, errno=%s", strerror(errno));
		return -1;
	}

	MEDIA.epollfd = epollfd;
	MEDIA.mediafd = mediafd;
	// MEDIA.localfd = localfd;

	// epoll_event_t event, evt2;
	// event.events = EPOLLIN;
	// event.data.ptr = &MEDIA;
	// epoll_ctl(epollfd, EPOLL_CTL_ADD, mediafd, &event);
	// evt2.events = EPOLLIN;
	// evt2.data.ptr = &MEDIA;
	// epoll_ctl(epollfd, EPOLL_CTL_ADD, localfd, &evt2);
	tunnel_t *tunnel;
	if ((tunnel = tunnel_create(mediafd, 1)) != NULL)
	{
		// tunnel_create(localfd, 0);
	}

	if (tunnel->priv_sock != NULL )
	{
		uint8_t header[3];
		header[0] = 0x05; // socks5
		header[1] = 0x01; // success
		header[2] = 0x00;
		printf("fd=%d send version\n", tunnel->priv_sock->fd);

		// 非标准 -- by wfz
		//  header[3] = 0x00; //none
		if (tunnel_write_client(tunnel, header, sizeof(header)) < 0)
			return -1;
	}

	snprintf(MEDIA.username, sizeof(MEDIA.username), "%s", username);
	snprintf(MEDIA.passwd, sizeof(MEDIA.passwd), "%s", passwd);

	return 0;
}

static void usage()
{
	fprintf(stderr,
			"Usage:\n"
			"-a : ip address\n"
			"-p : port \n"
			"-P : rmtPort \n"
			"-u<optional> : username\n"
			"-k<optional> : password\n");
}

int main(int n, char **args)
{
	sigign();

	char option;
	// char addr[128] = "";
	// char forRmtAddr[64] = "";
	char port[16] = "";
	// char locPort[16] = "";
	char username[255] = "";
	char passwd[255] = "";

	while ((option = getopt(n, args, "a:p:P:u:k:")) > 0)
	{
		switch (option)
		{
		case 'a':
			strncpy(g_locAddr, optarg, sizeof(g_locAddr));
			break;
		case 'p':
			strncpy(port, optarg, sizeof(port));
			break;
		// case 'A':
		// 	strncpy(forRmtAddr, optarg, sizeof(forRmtAddr));
		// 	break;
		case 'P':
			strncpy(g_locPort, optarg, sizeof(g_locPort));
			break;
		case 'u':
			strncpy(username, optarg, sizeof(username));
			break;
		case 'k':
			strncpy(passwd, optarg, sizeof(passwd));
			break;
		default:
			usage();
			break;
		}
	}

	if (strcmp(port, "") == 0 || strcmp(g_locAddr, "") == 0)
	{
		usage();
		return -1;
	}
	
	thrdListInit();

	if (medSvr_init(g_locAddr, port, NULL, username, passwd) < 0)
		return -1;
	if (media_start() < 0)
		return -1;

	return 0;
}

static int openConnSock(char *host, char *port, int *sockFd)
{

	// uint8_t atyp = tunnel->rp.atyp;
	char addr[128];
	char ip[64];
	// char port[16];

	// snprintf(port, sizeof(port),"%d", ntohs(tunnel->rp.port));
	// switch(atyp) {
	// 	case 0x01: // ipv4
	// 		inet_ntop(AF_INET, tunnel->rp.addr, ip, sizeof(ip));
	// 		addr = ip;
	// 		break;
	// 	case 0x04: // ipv6
	// 		inet_ntop(AF_INET6, tunnel->rp.addr, ip, sizeof(ip));
	// 		addr = ip;
	// 		break;
	// 	case 0x03: // domain
	// 		addr = tunnel->rp.addr;
	// 		break;
	// 	default:
	// 		assert(0);
	// 		break;
	// }
	sprintf(addr, "%s", host);

	addrinfo_t ai_hint;
	memset(&ai_hint, 0, sizeof(ai_hint));

	ai_hint.ai_family = AF_UNSPEC;
	ai_hint.ai_socktype = SOCK_STREAM;
	ai_hint.ai_protocol = IPPROTO_TCP;

	addrinfo_t *ai_list;
	addrinfo_t *ai_ptr;

	// TODO: getaddrinfo is a block function, try doing it in thread
	if (getaddrinfo(addr, port, &ai_hint, &ai_list) != 0)
	{
		LOG("getaddrinfo failed,addr=%s,port=%s,error=%s", addr, port, gai_strerror(errno));
		return -1;
	}

	int newfd = -1;
	int status;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next)
	{
		newfd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
		if (newfd < 0)
			continue;
		sock_nonblocking(newfd);
		sock_keepalive(newfd);

		if ((status = connect(newfd, ai_ptr->ai_addr, ai_ptr->ai_addrlen)) != 0 && errno != EINPROGRESS)
		{
			close(newfd);
			newfd = -1;
			continue;
		}

		break;
	}
	freeaddrinfo(ai_list);

	if (newfd < 0)
		return -1;

	*sockFd = newfd;
	printf("%s %s connect  ok.\n", host, port);

	return 0;
}

// --------------------------------------------------------------------------

int appCompare(connList_t *p1, connList_t *p2)
{
	// todo: 还要判断目标HOST
	if ( p1->tunnel->status == tunn_is_idle )
	// tunnel 状态已建立
		return 0;

	return -1;
}
int waitCompare(connList_t *p1, connList_t *p2)
{
	// todo: complete
	if ( p1->tunnel->status == tmptun_is_free )
		return 0;

	return -1;
}
connList_t *getWaitobjFrList(int rmtfd)
{
	connList_t tmpsvr, *p;

	if (waitListEmpty())
	{		
		goto new_obj;
	}

	//todo: boundary check -- WFZ
	p = waitListFnd(NULL,waitCompare);
	if ( p != NULL ) {
		p->tunnel->state = tunnel_waitting;
		p->tunnel->status = tmptun_is_wait;
		printf("rmtfd:%d use old-tmptun-obj, state=%d,status=%d\n", rmtfd,p->tunnel->state, p->tunnel->status);
		return p;
	}

	new_obj:
	p = (connList_t *)malloc(sizeof(connList_t));
	if (p == NULL)
	{
		LOG("malloc for waitListElem failed, errno=%s", strerror(errno));
		return NULL;
	}
	p->tunnel = (tunnel_t *)malloc(sizeof(tunnel_t));
	if (p->tunnel == NULL)
	{
		LOG("malloc for waitTunnel failed, errno=%s", strerror(errno));
		free(p);
		return NULL;
	}
	memset(p->tunnel, 0, sizeof(tunnel_t));
	p->tunnel->state = tunnel_waitting;
	p->tunnel->status = tmptun_is_wait;
	printf("rmtfd:%d use new-tmptun-obj, state=%d, status=%d\n", rmtfd,p->tunnel->state,p->tunnel->status);
	waitListAdd(p);

	return p;
}
connList_t *getTargetFrList(int rmtfd, int privflg)
{
	connList_t tmpsvr, *p;

	if (AppListEmpty())
	{		
		// if ( privflg  ==  0 )
		// 	return getWaitobjFrList(rmtfd);// 先有远程申请
		goto new_tun;
	}

	// if ( privflg  ==  0 ) 
	{
		memset(&tmpsvr, 0, sizeof(connList_t));
		// pthread_mutex_lock(&pub1_mutex);
		p = AppListFnd(&tmpsvr, appCompare);
		// pthread_mutex_unlock(&pub1_mutex);
		if ( p != NULL ) {
			p->tunnel->status = tunn_is_idle;
			printf("rmtfd:%d vs tunfd:%d use old-tunnel-lin, state=%d, status=%d\n", rmtfd, p->tunnel->priv_sock->fd,
				p->tunnel->state,p->tunnel->status);
			return p;
		}
		// else {
		// 	return getWaitobjFrList(rmtfd);//暂时无tunnel，先临时接收
		// }
	}

	new_tun:
	p = (connList_t *)malloc(sizeof(connList_t));
	if (p == NULL)
	{
		LOG("malloc for waitListElem failed, errno=%s", strerror(errno));
		return NULL;
	}
	p->tunnel = (tunnel_t *)malloc(sizeof(tunnel_t));
	if (p->tunnel == NULL)
	{
		LOG("malloc for waitTunnel failed, errno=%s", strerror(errno));
		free(p);
		return NULL;
	}
	memset(p->tunnel, 0, sizeof(tunnel_t));
	p->tunnel->status = tunn_is_idle; //tunn_is_idle;
	AppListAdd(p);
	printf("tunfd:%d use new-tunnel-lin, state=%d, status=%d\n", rmtfd,p->tunnel->state,p->tunnel->status);

	return p;
}

int waitCompare2(connList_t *p1, connList_t *p2)
{
	// todo: complete
	if ( p1->tunnel->status == tmptun_is_wait  || 
		(p1->tunnel->status == tmptun_is_alive && (p1->tunnel->remote_sock->read_buff->write_index -
		p1->tunnel->remote_sock->read_buff->read_index ) > 0 ) )
		return 0;

	return -1;
}

int fndIdleConnElemt(connList_t *ptr,void **arg)
{
	// connList_t *p;

	if ( ptr->tunnel->status == tmptun_is_alive ) {
		*arg = (void *)ptr;
		return 1;
	}

	//todo: retest here..fz
	return 0;
}


connList_t * waitLinePairing()
{
	connList_t  *p, *p1 = NULL;
	if (waitListEmpty())
	{		
		return NULL;
	}

	waitListDo(fndIdleConnElemt,(void **)&p1);
	return p1;
}
void parsingHeader(sock_t *sock)
{
	// char * p;
	// tunnel_t *tunnel = sock->tunnel;
	// size_t write_index;
	// size_t read_index;

	// if ( tunnel->remote_sock == NULL  )
	// 	return;

	// read_index = tunnel->remote_sock->read_buff->read_index;
	// write_index = tunnel->remote_sock->read_buff->write_index;
	// // printf("read req [%.*s]\n",tunnel->remote_sock->read_buff->write_index - tunnel->remote_sock->read_buff->read_index,
	// // 		tunnel->remote_sock->read_buff->data );
	// sock->iskeepalive = conn_keep_alive_noclear;
	// for ( p = tunnel->remote_sock->read_buff->data + read_index; 
	// 		p < tunnel->remote_sock->read_buff->data + write_index-read_index;  ) {
	// 	if ( strncasecmp(p,"Connection",10) == 0 )	{
	// 		p = p + 10;
	// 		do {
	// 			if ( *p == ':' ||  isspace(*p) ) {
	// 					p++;
	// 			}
	// 			else 
	// 				break;	
	// 		} while(1);

	// 		if ( strncasecmp(p,"keep-alive",10) == 0 )	{
	// 			sock->iskeepalive = conn_keep_alive;
	// 		}
	// 		break;
	// 	}
	// 	else {
	// 		// p = p + 10;
	// 		p = strchr(p,'\n');
	// 		if ( p == NULL )
	// 			break;
	// 		p = p + 1;
	// 	}	
	// }
	// printf("parsing keepalive:%d\n",sock->iskeepalive);
}


static int tunnel_connect_to_remote(tunnel_t *tunnel,char *addr, char *pPort)
{
	// uint8_t atyp = tunnel->rp.atyp;
	// char *addr;
	// char ip[64];
	char port[16];

	snprintf(port, sizeof(port),"%s", pPort);
	// switch(atyp) {
	// 	case 0x01: // ipv4
	// 		inet_ntop(AF_INET, tunnel->rp.addr, ip, sizeof(ip));
	// 		addr = ip;
	// 		break;
	// 	case 0x04: // ipv6
	// 		inet_ntop(AF_INET6, tunnel->rp.addr, ip, sizeof(ip));
	// 		addr = ip;
	// 		break;
	// 	case 0x03: // domain
	// 		addr = tunnel->rp.addr;
	// 		break;
	// 	default:
	// 		assert(0);
	// 		break;
	// }
	printf("new connect to real-server %s:%s\n", addr, port);

	addrinfo_t ai_hint;
	memset(&ai_hint, 0, sizeof(ai_hint));

	ai_hint.ai_family = AF_UNSPEC;
	ai_hint.ai_socktype = SOCK_STREAM;
	ai_hint.ai_protocol = IPPROTO_TCP;

	addrinfo_t *ai_list;
	addrinfo_t *ai_ptr;
	
	// TODO: getaddrinfo is a block function, try doing it in thread
	if (getaddrinfo(addr, port, &ai_hint, &ai_list) != 0) {
		LOG("getaddrinfo failed,addr=%s,port=%s,error=%s", addr, port, gai_strerror(errno));
		return -1;
	}

	int newfd = -1;
	int status;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
		newfd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
		if (newfd < 0) continue;
		sock_nonblocking(newfd);
		sock_keepalive(newfd);

		if ((status = connect(newfd, ai_ptr->ai_addr, ai_ptr->ai_addrlen)) != 0 && errno != EINPROGRESS) {
			close(newfd);
			newfd = -1;
			continue;
		}

		break;
	}
	freeaddrinfo(ai_list);

	if (newfd < 0) return -1;

	sock_t *sock = sock_create(newfd, sock_connecting, 0, tunnel);
	if (sock == NULL) {
		close(newfd);
		return -1;
	}
	tunnel->remote_sock = sock;
	tunnel->status = tunn_is_run;
	
	epoll_add(sock);
	// epoll_modify(sock, 1, 1);

	// if (status == 0) {
	// 	tunnel->state = connected_state;
	// 	sock->state = sock_connected;
	// 	return tunnel_notify_connected(tunnel);
	// } else {
	// 	tunnel->state = connecting_state;
	// 	sock->state = sock_connecting;
	// }
	
	return 0;
}
