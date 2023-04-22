#include <sys/time.h>
#include "osm.h"

#define LOOP_UNROLLING 5
#define MICRO_TO_NANO 1000
#define SEC_TO_NANO 1000000000

void empty_function() {}

int main(){

}
double osm_operation_time(unsigned int iterations) {
    if (!iterations){
        return -1;
    }
    int temp1 = 0, temp2 = 0, temp3 = 0, temp4 = 0, temp5 = 0;
    struct timeval start_time{};
    struct timeval end_time{};
    if (gettimeofday(&start_time, nullptr) == -1) {
        return -1;
    }
    for (int i=0; i < iterations; i++){
        temp1 += 1;
        temp2 += 1;
        temp3 += 1;
        temp4 += 1;
        temp5 += 1;
    }
    if (gettimeofday(&end_time, nullptr) == -1) {
        return -1;
    }
    double secs_to_nano = (double) (end_time.tv_sec - start_time.tv_sec ) * SEC_TO_NANO;
    double micro_secs_to_nano = (double)  (end_time.tv_usec - start_time.tv_usec ) * MICRO_TO_NANO;
    return (secs_to_nano + micro_secs_to_nano) / ((double) iterations * LOOP_UNROLLING);
}

__attribute__((unused)) double osm_function_time(unsigned int iterations) {
    if (!iterations){
        return -1;
    }
    struct timeval start_time{};
    struct timeval end_time{};
    if (gettimeofday(&start_time, nullptr) == -1) {
        return -1;
    }
    for (int i=0; i < iterations; i++){
        empty_function();
        empty_function();
        empty_function();
        empty_function();
        empty_function();
    }
    if (gettimeofday(&end_time, nullptr) == -1) {
        return -1;
    }
    double secs_to_nano = (double) (end_time.tv_sec - start_time.tv_sec ) * SEC_TO_NANO;
    double micro_secs_to_nano = (double)  (end_time.tv_usec - start_time.tv_usec ) * MICRO_TO_NANO;
    return (secs_to_nano + micro_secs_to_nano) / ((double) iterations * LOOP_UNROLLING);}

double osm_syscall_time(unsigned int iterations) {
    if (!iterations){
        return -1;
    }
    struct timeval start_time{};
    struct timeval end_time{};
    if (gettimeofday(&start_time, nullptr) == -1) {
        return -1;
    }
    for (int i=0; i < iterations; i++){
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
    }
    if (gettimeofday(&end_time, nullptr) == -1) {
        return -1;
    }
    double secs_to_nano = (double) (end_time.tv_sec - start_time.tv_sec ) * SEC_TO_NANO;
    double micro_secs_to_nano = (double)  (end_time.tv_usec - start_time.tv_usec ) * MICRO_TO_NANO;
    return (secs_to_nano + micro_secs_to_nano) / ((double) iterations * LOOP_UNROLLING);
}

