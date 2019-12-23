#ifndef __TIMER__
#define __TIMER__

#include <time.h>
#include <stdio.h>

#define TIMER_INIT()\
         struct timespec begin, end;\
         uint64_t dura;\

#define TIMER_UP()\
         clock_gettime(CLOCK_REALTIME, &begin);\

#define TIMER_DOWN()\
         clock_gettime(CLOCK_REALTIME, &end);\
         dura = ((uint64_t)end.tv_sec*1000000000 + end.tv_nsec)-((uint64_t)begin.tv_sec*1000000000 + begin.tv_nsec);\
         fprintf(stdout, "Time elapsed %lu ns\n", dura);\

#endif
