
#include "util.h"

static connList_t  g_svrlist, g_svrlist2;
static struct list_head *procs_head = NULL,*procs_head2 = NULL;

static sockList_t  g_sockfdlist;
static struct list_head *sockfdlist_head = NULL;
void AppListInit(void)
{
    if ( procs_head == NULL ) {
        memset(&g_svrlist,0,sizeof(g_svrlist));
        procs_head = &g_svrlist.list;
        INIT_LIST_HEAD(&g_svrlist.list);
        // return 0;
    }
}

int AppListAdd(connList_t * p)
{   
    list_add(&p->list,procs_head);
    return 0;
}

connList_t * AppListFnd(connList_t * p,cb_compare cbf)
{ 
    struct list_head *entry;
    connList_t *pptr;

    list_for_each(entry,procs_head) {
		pptr =  vbody_entry(entry, connList_t, list);
		if ( cbf(pptr,p) == 0 )
            return pptr;
	}

    return NULL;
}

void AppListDo(cb_dosth cbf,void **arg)
{ 
    struct list_head *entry;
    connList_t *pptr;

    list_for_each(entry,procs_head) {
		pptr =  vbody_entry(entry, connList_t, list);
		if ( cbf(pptr,arg) == -1 )
            break;
	}

    return ;
}

connList_t * AppListGet(connList_t * p,cb_compare cbf)
{ 
    connList_t *pptr;

    pptr = AppListFnd(p,cbf);
    if (pptr != NULL) {
		list_del(&pptr->list);
        return pptr;
	}

    return NULL;
}


// int appCompare(connList_t * p1,connList_t * p2)
// { 
//     //todo: complete

//     return 0;
// }

int AppListUpd(connList_t * p,cb_compare cbf) {
    connList_t * ptr;
    ptr = AppListFnd( p, cbf);
    if ( ptr != NULL ) {
        memcpy((void*)ptr,(void*)p,sizeof(connList_t));
        return 1;
    }
    return 0;
}

int  AppListEmpty( void ) {
    return list_empty(procs_head);
}

// ---------------------------------------------------------------------------

void waitListInit(void)
{
    if ( procs_head2 == NULL ) {
        memset(&g_svrlist2,0,sizeof(g_svrlist2));
        procs_head2 = &g_svrlist2.list;
        INIT_LIST_HEAD(&g_svrlist2.list);
        // return 0;
    }
}

int waitListAdd(connList_t * p)
{
    list_add(&p->list,procs_head2);
    return 0;
}

connList_t * waitListFnd(connList_t * p,cb_compare cbf)
{ 
    struct list_head *entry;
    connList_t *pptr;

    list_for_each(entry,procs_head2) {
		pptr =  vbody_entry(entry, connList_t, list);
		if ( cbf(pptr,p) == 0 )
            return pptr;
	}

    return NULL;
}
void waitListDo(cb_dosth cbf,void **arg)
{ 
    struct list_head *entry;
    connList_t *pptr;

    list_for_each(entry,procs_head2) {
		pptr =  vbody_entry(entry, connList_t, list);
		if ( cbf(pptr,arg) == 1 )
            break;
	}

    return ;
}
connList_t * waitListGet(connList_t * p,cb_compare cbf)
{ 
    connList_t *pptr;

    pptr = waitListFnd(p,cbf);
    if (pptr != NULL) {
		list_del(&pptr->list);
        return pptr;
	}

    return NULL;
}

connList_t * waitListDel1(tunnel_t * pt)
{ 
    connList_t *pptr;

    // pptr = waitListFnd(p,cbf);
    // if (pptr != NULL) {
	// 	list_del(&pptr->list);
    //     return pptr;
	// }
    pptr =  vbody_entry(pt, connList_t, tunnel);
    list_del(&pptr->list);

    return pptr;
}

int waitListUpd(connList_t * p,cb_compare cbf) {
    connList_t * ptr;
    ptr = waitListFnd( p, cbf);
    if ( ptr != NULL ) {
        memcpy((void*)ptr,(void*)p,sizeof(connList_t));
        return 1;
    }
    return 0;
}

int  waitListEmpty( void ) {
    return list_empty(procs_head2);
}

// ---------------------------------------------------------------------------
void FdListInit(void)
{
    if ( sockfdlist_head == NULL ) {
        memset(&g_sockfdlist,0,sizeof(g_sockfdlist));
        sockfdlist_head = &g_sockfdlist.list;
        INIT_LIST_HEAD(&g_sockfdlist.list);
        // return 0;
    }
}

int FdListAdd(sockList_t * p)
{
    list_add(&p->list,sockfdlist_head);
    return 0;
}

sockList_t * FdListFnd(sockList_t * p,fd_compare cbf)
{ 
    struct list_head *entry;
    sockList_t *pptr;

    list_for_each(entry,sockfdlist_head) {
		pptr =  vbody_entry(entry, sockList_t, list);
		if ( cbf(pptr,p) == 0 )
            return pptr;
	}

    return NULL;
}

sockList_t * FdListGet(sockList_t * p,fd_compare cbf)
{ 
    sockList_t *pptr;

    pptr = FdListFnd(p,cbf);
    if (pptr != NULL) {
		list_del(&pptr->list);
        return pptr;
	}

    return NULL;
}


// int appCompare(connList_t * p1,connList_t * p2)
// { 
//     //todo: complete

//     return 0;
// }

// int FdListUpd(connList_t * p,fd_compare cbf) {
//     connList_t * ptr;
//     ptr = FdListFnd( p, cbf);
//     if ( ptr != NULL ) {
//         memcpy((void*)ptr,(void*)p,sizeof(connList_t));
//         return 1;
//     }
//     return 0;
// }

int  FdListEmpty( void ) {
    return list_empty(sockfdlist_head);
}

// ---------------------------------------------------
void thrdListInit(void)
{
    AppListInit();
    waitListInit();
    FdListInit();
}