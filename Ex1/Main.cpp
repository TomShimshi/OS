//
// Created by Tom Shimshi on 21/03/2023.
//

#include "osm.h"

int main() {
    unsigned int iterations = 10000;
    const double op = osm_operation_time(iterations);
    const double func = osm_function_time(iterations);
    const double sys = osm_syscall_time(iterations);
    std::cout << 'op- ' << op << ', func- ' << func << ', sys- ' << sys << std::endl;
}