#include <stdio.h>

extern "C" void native_method()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("<stdout_marker>Native: Start\n");
    printf("<stdout_marker>Native: End\n");                     // BREAK2
}
