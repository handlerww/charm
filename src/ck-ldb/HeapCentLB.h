/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#ifndef _HEAPCENTLB_H_
#define _HEAPCENTLB_H_

#include "CentralLB.h"
#include "HeapCentLB.decl.h"

void CreateHeapCentLB();

class HeapCentLB : public CentralLB {

public:
  struct HeapData {
    double load;
    int    pe;
    int    id;
  };

  HeapCentLB();
private:
  void           Heapify(HeapData*, int, int);
  void           HeapSort(HeapData*, int);
	void           BuildHeap(HeapData*, int);
  HeapData*      BuildCpuArray(CentralLB::LDStats*, int, int *);      
  HeapData*      BuildObjectArray(CentralLB::LDStats*, int, int *);      
  CmiBool        QueryBalanceNow(int step);
  CLBMigrateMsg* Strategy(CentralLB::LDStats* stats, int count);
};

#endif /* _HEAPCENTLB_H_ */
