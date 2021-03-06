#include <lib.h>
#include <getrlimit.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <stdio.h>

PUBLIC int getrlimit(int resource, struct rlimit *rlim){
	int r = 0;
	message m;
	if(rlim == NULL)
		return(EFAULT);
	if(sizeof(resource) != sizeof(int))
		return(EINVAL);
	m.m1_i1 = resource;
	switch (resource) {
		case RLIMIT_NICE   :  r = _syscall(PM_PROC_NR, GET_RES_LIMIT,&m); break;
		case RLIMIT_NPROC  :  r = _syscall(PM_PROC_NR, GET_RES_LIMIT,&m); break;
		case RLIMIT_NOFILE :  r = _syscall(VFS_PROC_NR,GET_RES_LIMIT,&m); break;
		case RLIMIT_FSIZE  :  r = _syscall(VFS_PROC_NR,GET_RES_LIMIT,&m); break;
        	default 	   :  r = -1;  			      		  break;
        }
	rlim->rlim_cur = m.m2_l1;
        rlim->rlim_max = m.m2_l2;   

   return (r);
}
