/*
 Interface to Converse portion of parallel debugger.
 Moved here from converse.h 4/10/2001 by Orion Sky Lawlor, olawlor@acm.org
 */
#ifndef __CMK_DEBUG_CONV_H
#define __CMK_DEBUG_CONV_H

#include "pup_c.h"

#ifdef __cplusplus
extern "C" {
#endif

void CpdInit(void); 
void CpdFreeze(void);  
void CpdUnFreeze(void);
void CpdFreezeModeScheduler(void);
void CpdStartGdb(void);
void Cpd_CmiHandleMessage(void *msg);

/* C bindings for CpdList functions: */

/**
  When a CCS client asks for some data in a CpdList, the
  system generates this struct to describe the range of 
  items the client asked for (the items are numbered lo to hi-1),
  as well as store any extra data the CCS client passed in.
*/
typedef struct {
	int lo,hi; /**< Range of requested items in list is (lo .. hi-1)*/
	int extraLen; /**< Amount of data pointed to below*/
	void *extra; /**< List-defined request data shipped in via CCS */
} CpdListItemsRequest;

/**
 Call this routine at the start of each CpdList item.
 This lets the client distinguish one item from the next.
*/
void CpdListBeginItem(pup_er p,int itemNo);

/**
 User-written C routine to pup a range of items in a CpdList.
    \param itemsParam User-defined parameter passed to CpdListRegister_c.
    \param p pup_er to pup items to.
    \param req Cpd request object, describing items to pup.
*/
typedef void (*CpdListItemsFn_c)(void *itemsParam,pup_er p,
				CpdListItemsRequest *req);

/**
  User-written C routine to return the length (number of items) 
  in this CpdList.
    \param lenParam User-defined parameter passed to CpdListRegister_c.
    \param return Length of the CpdList.
*/
typedef int  (*CpdListLengthFn_c)(void *lenParam);

/**
  Create a new CpdList at the given path.  When a CCS client requests
  this CpdList, Cpd will use these user-written C routines to extract 
  the list's length and items.
    \param path CpdList request path.  The CCS client passes in this path.
    \param lenFn User-written subroutine to calculate the list's current length.
    \param lenParam User-defined parameter passed to lenFn.
    \param itemsFn User-written subroutine to pup the list's items.
    \param itemsParam User-defined parameter passed to itemsFn.
*/
void CpdListRegister_c(const char *path,
	    CpdListLengthFn_c lenFn,void *lenParam,
	    CpdListItemsFn_c itemsFn,void *itemsParam);

#ifdef __cplusplus
};
#endif

#endif
