/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#ifndef _CK_H_
#define _CK_H_

#include <string.h>
#include <stdlib.h>

#ifdef CMK_OPTIMIZE
#define NDEBUG
#endif

#include <assert.h>

#include "charm.h"

#include "envelope.h"
#include "init.h"
#include "qd.h"
#include "register.h"
#include "stats.h"
#include "waitqd.h"
#include "ckfutures.h"
#include "ckarray.h"
#include "ckstream.h"

#ifndef CMK_OPTIMIZE
#define _CHECK_VALID(p, msg) do {if((p)==0){CkAbort(msg);}} while(0)
#else
#define _CHECK_VALID(p, msg) do { } while(0)
#endif

class VidBlock {
    enum VidState {FILLED, UNFILLED};
    VidState state;
    PtrQ *msgQ;
    CkChareID actualID;
  public:
    VidBlock() { state = UNFILLED; msgQ = new PtrQ(); _MEMCHECK(msgQ); }
    void send(envelope *env) {
      if(state==UNFILLED) {
        msgQ->enq((void *)env);
      } else {
        env->setSrcPe(CkMyPe());
        env->setMsgtype(ForChareMsg);
        env->setObjPtr(actualID.objPtr);
        CldEnqueue(actualID.onPE, env, _infoIdx);
        CpvAccess(_qd)->create();
      }
    }
    void fill(int onPE, void *oPtr, int magic) {
      state = FILLED;
      actualID.onPE = onPE;
      actualID.objPtr = oPtr;
      actualID.magic = magic;
      envelope *env;
      while(env=(envelope*)msgQ->deq()) {
        env->setSrcPe(CkMyPe());
        env->setMsgtype(ForChareMsg);
        env->setObjPtr(actualID.objPtr);
        CldEnqueue(actualID.onPE, env, _infoIdx);
        CpvAccess(_qd)->create();
      }
      delete msgQ; msgQ=0;
    }
};

extern void _processHandler(void *);
extern void _infoFn(void *msg, CldPackFn *pfn, int *len,
                    int *queueing, int *priobits, UInt **prioptr);
extern void _packFn(void **msg);
extern void _unpackFn(void **msg);
extern void _createGroupMember(CkGroupID groupID, int eIdx, void *env);
extern void _createNodeGroupMember(CkGroupID groupID, int eIdx, void *env);
extern void _createGroup(CkGroupID groupID, envelope *env, int retEp, 
                         CkChareID *retChare);
extern void _createNodeGroup(CkGroupID groupID, envelope *env, int retEp,
                             CkChareID *retChare);
#endif
