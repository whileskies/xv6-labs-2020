#include "kernel/types.h"
#include "user.h"

int
main(int argc, char *argv[])
{
    uint64 xticks = uptime();
    int second = xticks / 10 % 60;
    int minute = xticks / 10 / 60 % 60;
    int hour = xticks / 10 / 60 / 60 % 24;
    int day = xticks / 10 / 60 / 60 / 24;
    printf("%d ticks, up %d days %d:%d:%d\n", xticks, day, hour, minute, second);

    exit(0);
}