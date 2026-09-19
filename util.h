#ifndef __UTIL_H__
#define __UTIL_H__ 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mysocks5.h"
#include "list.h"
#include "ventry.h"

typedef struct conn_List_T {
	tunnel_t *tunnel;
	list_head list;
} connList_t ;

typedef struct sock_List_T {
	int fd;
	list_head list;
} sockList_t ;

typedef int (*cb_compare)(connList_t *p1, connList_t *p2) ;
typedef int (*cb_dosth)(connList_t *p1,  void **arg) ;
typedef int (*fd_compare)(sockList_t *p1, sockList_t *p2) ;

int AppListAdd(connList_t * p);
connList_t * AppListFnd(connList_t * p,cb_compare cbf);
int AppListUpd(connList_t * p,cb_compare cbf);int  AppListEmpty( void );
void AppListDo(cb_dosth cbf,void **arg);
connList_t * AppListGet(connList_t * p,cb_compare cbf);
int  AppListEmpty( void );
int appCompare(connList_t * p1,connList_t * p2);


int waitListAdd(connList_t * p);
connList_t * waitListFnd(connList_t * p,cb_compare cbf);
int waitListUpd(connList_t * p,cb_compare cbf);int  AppListEmpty( void );
void waitListDo(cb_dosth cbf,void **arg);
connList_t * waitListGet(connList_t * p,cb_compare cbf);
connList_t * waitListDel1(tunnel_t * pt);
int  waitListEmpty( void );


int FdListAdd(sockList_t * p);
sockList_t * FdListFnd(sockList_t * p,fd_compare cbf);
sockList_t * FdListGet(sockList_t * p,fd_compare cbf);
int  FdListEmpty( void );

void thrdListInit(void);
#endif