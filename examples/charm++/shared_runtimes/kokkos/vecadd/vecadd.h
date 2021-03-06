#ifndef VECADD_H_
#define VECADD_H_
#include <cstdint>

void kokkosInit();
void kokkosInit(int device_id);
void kokkosFinalize();
void vecadd(const uint64_t n, int process, bool use_gpu);

#endif // VECADD_H_
