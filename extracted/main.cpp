#include "kernel.h"
#include <cmath>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/uio.h>


//用于获取一个进程中所有的task
bool GetProcessTask(int pid, std::vector<int> & vOutput) {
	char szTaskPath[256];
	sprintf(szTaskPath, "/proc/%d/task", pid);
	DIR *dir = opendir(szTaskPath);
	if (!dir) return false;
	
    struct dirent *ptr;
	while ((ptr = readdir(dir)) != NULL) {
		if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0)) continue;
		if (ptr->d_type != DT_DIR) continue;
		if (strspn(ptr->d_name, "1234567890") != strlen(ptr->d_name)) continue;
		vOutput.push_back(atoi(ptr->d_name));
	}
	closedir(dir);
	return true;
}


int main(int argc, char const *argv[]) {
    c_driver driver;
    
    //检查驱动是否成功对接
    if (!driver.is_ready()) {
        fprintf(stderr, "driver is not ready\n");
        return 1;
    }
    
    //检查断点是否初始化，若没有初始化，则初始化
    if (!driver.bp_check_inited())
    {
        driver.bp_init_driver();
    }
    
    pid_t target_pid = driver.get_name_pid("com.proxima.dfm");
    driver.initialize(target_pid);
    uintptr_t UE4 = driver.get_module_base("libUE4.so");
    
    printf("pid: %d,UE4: %lx\n",target_pid,UE4);
    
    uintptr_t ptr = driver.read_v2<uintptr_t>(UE4);
    printf("ptr: %lx\n",ptr);
    return 0;
}
