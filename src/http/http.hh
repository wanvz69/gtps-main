#ifndef __HTTP__HTTP__HH__
#define __HTTP__HTTP__HH__

#include <atomic>

#include <sandbird.h>

namespace beef::http
{
    int handler(sb_Event* e);

    void serve(std::atomic<bool>& running);
}

#endif // __HTTP__HTTP__HH__