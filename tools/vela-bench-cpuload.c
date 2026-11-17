// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIMIT 32
#define CPU_LIMIT 65536
static volatile sig_atomic_t stopping;
static uint64_t stamp(clockid_t clock)
{
    struct timespec t;
    if(clock_gettime(clock,&t))_exit(1);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static void stop(int signal) { (void)signal;stopping=1; }
static void expire(int signal) { (void)signal;_exit(124); }
static void worker(int index,int cpu,int gate,pid_t parent,size_t size)
{
    if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(1);
    cpu_set_t *set=CPU_ALLOC(CPU_LIMIT);
    if(!set)_exit(1);
    CPU_ZERO_S(size,set);CPU_SET_S(cpu,size,set);
    if(sched_setaffinity(0,size,set))_exit(1);
    CPU_FREE(set);
    char byte;
    if(read(gate,&byte,1)!=1)_exit(1);
    close(gate);
    uint64_t chunks=0,sequence=0,value=0x123456789abcdefull,last=0;
    for (;;) {
        for(unsigned i=0;i<65536;++i) {value^=value<<13;value^=value>>7;value^=value<<17;}
        ++chunks;
        const int observedStop = stopping;
        uint64_t now=stamp(CLOCK_MONOTONIC);
        if(now-last>=500000000ull || observedStop) {
            if(sched_getcpu()!=cpu)_exit(1);
            char record[512];
            int n=snprintf(record,sizeof record,"{\"event\":\"cpu-load-sample\",\"worker\":%d,\"pid\":%d,\"cpu\":%d,\"sequence\":%" PRIu64
                ",\"chunks\":%" PRIu64 ",\"checksum\":\"%016" PRIx64 "\",\"cpu_time_ns\":%" PRIu64 ",\"monotonic_ns\":%" PRIu64 "}\n",
                index,getpid(),cpu,++sequence,chunks,value,stamp(CLOCK_PROCESS_CPUTIME_ID),now);
            if(n<=0 || n>=(int)sizeof record || write(STDOUT_FILENO,record,(size_t)n)!=n)_exit(1);
            last=now;
        }
        if (observedStop) break;
    }
    _exit(0);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--help")) {
        puts("Usage: vela-bench-cpuload --json --until-stdin-eof\nFixed CPU contention: one pinned integer worker per allowed CPU, capped at 32; 110 s deadline; EOF stops all owned workers.");return 0;
    }
    if(argc!=3 || strcmp(argv[1],"--json") || strcmp(argv[2],"--until-stdin-eof") || !getuid() || !geteuid())return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGUSR1,stop);signal(SIGALRM,expire);alarm(110);
    size_t size=CPU_ALLOC_SIZE(CPU_LIMIT);cpu_set_t *set=CPU_ALLOC(CPU_LIMIT);
    if(!set)return 1;
    CPU_ZERO_S(size,set);
    if(sched_getaffinity(0,size,set)) {CPU_FREE(set);return 1;}
    int cpus[LIMIT],count=0,allowed=CPU_COUNT_S(size,set);
    for(int cpu=0;cpu<CPU_LIMIT && count<LIMIT;++cpu)if(CPU_ISSET_S(cpu,size,set))cpus[count++]=cpu;
    CPU_FREE(set);if(!count)return 1;
    int gate[2];if(pipe(gate))return 1;
    pid_t parent=getpid(),children[LIMIT];int made=0,failed=0,eof=0;
    for(int index=0;index<count;++index) {
        pid_t pid=fork();
        if(pid<0) {failed=1;break;}
        if(!pid) {close(gate[1]);worker(index,cpus[index],gate[0],parent,size);}
        children[made++]=pid;
    }
    close(gate[0]);
    printf("{\"event\":\"cpu-load\",\"schema\":1,\"method\":\"pinned-integer-contention-v1\",\"clock\":\"CLOCK_MONOTONIC\",\"uid\":%u,\"pid\":%d,\"allowed_cpus\":%d,\"workers\":[",getuid(),getpid(),allowed);
    for(int i=0;i<made;++i)printf("%s{\"worker\":%d,\"pid\":%d,\"cpu\":%d}",i?",":"",i,children[i],cpus[i]);
    puts("],\"worker_cap\":32,\"iterations_per_chunk\":65536,\"sample_period_ns\":500000000}");fflush(stdout);
    char bytes[LIMIT];memset(bytes,1,sizeof bytes);
    if(!failed && write(gate[1],bytes,(size_t)made)!=made)failed=1;
    close(gate[1]);
    while(!stopping && !failed && !eof) {
        struct pollfd input={STDIN_FILENO,POLLIN,0};int ready=poll(&input,1,100);
        if(ready<0 && errno!=EINTR) {failed=1;break;}
        if(input.revents&(POLLIN|POLLHUP|POLLERR)) {char byte;ssize_t n=read(0,&byte,1);if(n==0)eof=1;else failed=1;}
        for(int i=0;i<made;++i)if(waitpid(children[i],NULL,WNOHANG)!=0) {children[i]=0;failed=1;}
    }
    uint64_t end_request=stamp(CLOCK_MONOTONIC);
    for(int i=0;i<made;++i)if(children[i]>0)kill(children[i],SIGUSR1);
    for(int i=0;i<made;++i)if(children[i]>0) {
        int status=0;pid_t done;
        do {done=waitpid(children[i],&status,0);}while(done<0 && errno==EINTR);
        if(done!=children[i] || !WIFEXITED(status) || WEXITSTATUS(status))failed=1;
    }
    printf("{\"event\":\"cpu-load-summary\",\"complete\":%s,\"workers_reaped\":%d,\"end_request_monotonic_ns\":%" PRIu64 ",\"monotonic_ns\":%" PRIu64 "}\n",
        !failed && eof && !stopping?"true":"false",made,end_request,stamp(CLOCK_MONOTONIC));
    return failed || stopping || !eof?1:0;
}
