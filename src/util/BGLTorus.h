/** \file BGLTorus.h
 *  Author: Abhinav S Bhatele
 *  Date created: June 28th, 2007  
 *  The previous file bgltorus.h was cleaned up.
 */

#ifndef _BGL_TORUS_H_
#define _BGL_TORUS_H_

#include <string.h>
#include "converse.h"

#if CMK_VERSION_BLUEGENE

#include <bglpersonality.h>

extern "C" int rts_get_personality(struct BGLPersonality *dst, unsigned size);
extern "C" int rts_coordinatesForRank(unsigned logicalRank, unsigned *x,
                                  unsigned *y, unsigned *z, unsigned *t);

class BGLTorusManager {
  private:
    BGLPersonality bgl_p;
    int dimX;	// dimension of the allocation in X (no. of processors)
    int dimY;	// dimension of the allocation in Y (no. of processors)
    int dimZ;	// dimension of the allocation in Z (no. of processors)
    int dimNX;	// dimension of the allocation in X (no. of nodes)
    int dimNY;	// dimension of the allocation in Y (no. of nodes)
    int dimNZ;	// dimension of the allocation in Z (no. of nodes)
    int dimNT;  // dimension of the allocation in T (no. of processors per node)
   
    int torus[4];
    int procsPerNode;
    char *mapping;

  public:
    BGPTorusManager() {
      int size = sizeof(BGLPersonality);
      int i = rts_get_personality(&bgl_p, size);

      dimNX = bgl_p.xSize;
      dimNY = bgl_p.ySize;
      dimNZ = bgl_p.zSize;
   
      CkPrintf("%d %d %d\n", dimNX, dimNY, dimNZ);
      if(bgl_p.opFlags & BGLPERSONALITY_OPFLAGS_VIRTUALNM)
        dimNT = 2;
      else
	dimNT = 1;

      dimX = dimNX;
      dimY = dimNY;
      dimZ = dimNZ;
 
      dimX = dimX * dimNT;	// assuming TXYZ
      procsPerNode = dimNT;

      torus[0] = bgl_p.isTorusX();
      torus[1] = bgl_p.isTorusY();
      torus[2] = bgl_p.isTorusZ();
      
      mapping = (char *)malloc(sizeof(char)*4);
      mapping = getenv("BGLMPI_MAPPING");
    }

    ~BGLTorusManager() { 
     }

    inline int getDimX() { return dimX; }
    inline int getDimY() { return dimY; }
    inline int getDimZ() { return dimZ; }

    inline int getDimNX() { return dimNX; }
    inline int getDimNY() { return dimNY; }
    inline int getDimNZ() { return dimNZ; }
    inline int getDimNT() { return dimNT; }

    inline int getProcsPerNode() { return procsPerNode; }

    inline int* isTorus() { return torus; }

    inline void rankToCoordinates(int pe, int &x, int &y, int &z) {
      x = pe % dimX;
      y = (pe % (dimX*dimY)) / dimX;
      z = pe / (dimX*dimY);
    }

    inline void rankToCoordinates(int pe, int &x, int &y, int &z, int &t) {
      /*if(mapping!=NULL && strcmp(mapping, "XYZT")) {
        x = pe % dimNX;
        y = (pe % (dimNX*dimNY)) / dimNX;
        z = (pe % (dimNX*dimNY*dimNZ)) / (dimNX*dimNY);
        t = pe / (dimNX*dimNY*dimNZ);
      } else {*/
        t = pe % dimNT;
        x = (pe % (dimNT*dimNX)) / dimNT;
        y = (pe % (dimNT*dimNX*dimNY)) / (dimNT*dimNX);
        z = pe / (dimNT*dimNX*dimNY);
      //}
    }

    inline int coordinatesToRank(int x, int y, int z) {
      return x + y*dimX + z*dimX*dimY;
    }

    inline int coordinatesToRank(int x, int y, int z, int t) {
      /*if(mapping!=NULL && strcmp(mapping, "XYZT"))
        return x + y*dimNX + z*dimNX*dimNY + t*dimNX*dimNY*dimNZ;
      else*/
        return t*(dimNT-1) + x*dimNT + y*dimNT*dimNX + z*dimNT*dimNX*dimNY;
    }
};

#endif // CMK_VERSION_BLUEGENE
#endif //_BGL_TORUS_H_
