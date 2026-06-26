#pragma once

#include <cstdint>
#include <dirent.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <initializer_list>
#include <utility>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/types.h>
#include <termios.h>
#include <time.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <vector>
#include <unordered_map>

#include <sys/syscall.h>

#define MY_CALL(magic1, magic2, cmd, arg) ({    \
    long _ret;                                             \
    register long _x0 __asm__("x0") = (long)(magic1);      \
    register long _x1 __asm__("x1") = (long)(magic2);      \
    register long _x2 __asm__("x2") = (long)(cmd);         \
    register long _x3 __asm__("x3") = (long)(arg);         \
    register long _nr __asm__("x8") = __NR_reboot;         \
    __asm__ __volatile__(                                  \
        "svc #0"                                           \
        : "=r"(_x0)                                        \
        : "r"(_x0), "r"(_x1), "r"(_x2), "r"(_x3), "r"(_nr)\
        : "memory", "cc"                                   \
    );                                                     \
    _ret = _x0;                                            \
    _ret;                                                  \
})

#define TWT_MARK 'T'
#define GET_PID         _IOW(TWT_MARK, 0, _request)
#define MODULE_BASE     _IOW(TWT_MARK, 1, _request)
#define MODULE_BSS      _IOW(TWT_MARK, 3, _request)
#define READ_MEM        _IOW(TWT_MARK, 4, _request)
#define READ_MEM_V2     _IOW(TWT_MARK, 11, _request)
#define WRITE_MEM       _IOW(TWT_MARK, 5, _request)
#define TOUCH_INIT      _IOW(TWT_MARK, 6, touch_event_base)
#define TOUCH_DOWN      _IOW(TWT_MARK, 7, touch_event_base)
#define TOUCH_UP        _IOW(TWT_MARK, 8, touch_event_base)
#define GYRO_INIT       _IOW(TWT_MARK, 9, int)
#define GYRO_CONFIG     _IOWR(TWT_MARK, 10, gyro_config)

#define BP_INIT_CMD        _IO(TWT_MARK, 19)
#define BP_CHECK_INITED    _IO(TWT_MARK, 30)
#define BP_GET_NUM_BRPS    _IO(TWT_MARK, 20)
#define BP_GET_NUM_WRPS    _IO(TWT_MARK, 21)
#define BP_INST            _IOWR(TWT_MARK, 22, char *)
#define BP_UNINST          _IOW(TWT_MARK, 23, char *)
#define BP_SUSPEND         _IOW(TWT_MARK, 24, char *)
#define BP_RESUME          _IOW(TWT_MARK, 25, char *)
#define BP_GET_HIT_COUNT   _IOWR(TWT_MARK, 26, char *)
#define BP_MODIFY          _IOW(TWT_MARK, 27, char *)
#define BP_GET_HIT_ITEMS   _IOWR(TWT_MARK, 28, char *)

#define HW_BREAKPOINT_LEN_1 1
#define HW_BREAKPOINT_LEN_2 2
#define HW_BREAKPOINT_LEN_3 3
#define HW_BREAKPOINT_LEN_4 4
#define HW_BREAKPOINT_LEN_5 5
#define HW_BREAKPOINT_LEN_6 6
#define HW_BREAKPOINT_LEN_7 7
#define HW_BREAKPOINT_LEN_8 8

#define HW_BREAKPOINT_EMPTY   0
#define HW_BREAKPOINT_R       1
#define HW_BREAKPOINT_W       2
#define HW_BREAKPOINT_RW      (HW_BREAKPOINT_R | HW_BREAKPOINT_W)
#define HW_BREAKPOINT_X       4

#define REG_MODIFY_X(N)     (1ULL << (N))
#define REG_MODIFY_SP     (1ULL << 31)
#define REG_MODIFY_PC     (1ULL << 32)
#define REG_MODIFY_PSTATE (1ULL << 33)

#define BP_FLAG_RECORD    0x1

#pragma pack(1)
struct bp_user_pt_regs
{
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
    __uint128_t vregs[32];
};

struct bp_hit_item
{
    uint64_t task_id;
    uint64_t hit_addr;
    uint64_t hit_time;
    struct bp_user_pt_regs regs_info;
};

struct bp_get_hit_count_arg
{
    uint64_t handle;
    uint64_t hit_total_count;
    uint64_t hit_item_arr_count;
};

struct bp_get_hit_items_args
{
    uint64_t handle;
    uint64_t user_buffer_ptr;
    uint64_t max_bytes;
    uint64_t items_copied;
};

struct bp_inst_args
{
    int32_t pid;
    uint64_t addr;
    uint32_t bp_len;
    uint32_t bp_type;
    uint64_t reg_modify_mask;
    uint64_t fp_reg_modify_mask;
    uint64_t regs_to_set_ptr;
    uint32_t flags;
};

struct bp_modify_args
{
    uint64_t handle;
    uint64_t reg_modify_mask;
    uint64_t fp_reg_modify_mask;
    uint64_t regs_to_set_ptr;
};
#pragma pack()

struct bp_reg_batch
{
    bp_user_pt_regs regs = {};
    uint64_t reg_mask = 0;
    uint64_t fp_mask = 0;

    bp_reg_batch &x(int idx, uint64_t val)
    {
        if (idx >= 0 && idx <= 30) {
            regs.regs[idx] = val;
            reg_mask |= 1ULL << idx;
        }
        return *this;
    }

    bp_reg_batch &sp(uint64_t val)
    {
        regs.sp = val;
        reg_mask |= REG_MODIFY_SP;
        return *this;
    }

    bp_reg_batch &pc(uint64_t val)
    {
        regs.pc = val;
        reg_mask |= REG_MODIFY_PC;
        return *this;
    }

    bp_reg_batch &pstate(uint64_t val)
    {
        regs.pstate = val;
        reg_mask |= REG_MODIFY_PSTATE;
        return *this;
    }

    bp_reg_batch &vf(int idx, float val)
    {
        if (idx >= 0 && idx <= 31) {
            __uint128_t v = 0;
            memcpy(&v, &val, sizeof(float));
            regs.vregs[idx] = v;
            fp_mask |= 1ULL << idx;
        }
        return *this;
    }

    bp_reg_batch &vd(int idx, double val)
    {
        if (idx >= 0 && idx <= 31) {
            __uint128_t v = 0;
            memcpy(&v, &val, sizeof(double));
            regs.vregs[idx] = v;
            fp_mask |= 1ULL << idx;
        }
        return *this;
    }

    bp_reg_batch &reset()
    {
        memset(&regs, 0, sizeof(regs));
        reg_mask = 0;
        fp_mask = 0;
        return *this;
    }

    bool empty() const { return reg_mask == 0 && fp_mask == 0; }
};

class c_driver
{
private:
    int fd = -1;
    pid_t pid = -1;

    typedef struct _request
    {
        pid_t pid;
        uintptr_t addr;
        void *buffer;
        size_t size;
    } request, *Prequest;

    typedef struct _touch_event_base
    {
        int slot;
        int x;
        int y;
    } touch_event_base, *Ptouch_event_base;

    typedef struct gyro_config {
        uint32_t enable;
        uint32_t x;
        uint32_t y;
    } gyro_config, *Pgyro_config;

    __attribute__((visibility("hidden"))) int getFd(const char *str)
    {
        DIR *dir;
        struct dirent *entry;
        char path[PATH_MAX], link[PATH_MAX];
        int found_fd = -1;

        dir = opendir("/proc/self/fd");
        if (!dir)
        {
            return -1;
        }

        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);

            ssize_t len = readlink(path, link, sizeof(link) - 1);
            if (len == -1)
            {
                continue;
            }
            link[len] = '\0';

            if (strstr(link, str) != NULL && strstr(link, "anon_inode:") != NULL)
            {
                found_fd = atoi(entry->d_name);
                break;
            }
        }
        closedir(dir);

        return found_fd;
    }
   
public:
    c_driver()
    {
        fd = -1;
        MY_CALL(0x114514, 0x1919810, 0x2778, &fd);
        if (fd < 0)
            fd = getFd("TwT_driver");
        if (fd < 0)
        {
            printf("驱动对接失败\n");
            exit(0);
        } else {
            printf("驱动对接成功\n");
        }

        int choice = -1;

        printf("\n===== 陀螺仪初始化 =====\n");
        printf("0: tracepoint 方式\n");
        printf("1: uprobe 方式\n");
        printf("2: 不启用陀螺仪\n");
        printf("请选择: ");
        scanf("%d", &choice);
        if (choice >= 0 && choice <= 1) {
            if (gyro_init(choice))
                printf("陀螺仪初始化成功 (模式 %d)\n", choice);
            else
                printf("陀螺仪初始化失败\n");
        } else {
            printf("跳过陀螺仪初始化\n");
        }

        printf("\n===== 触摸初始化 =====\n");
        printf("0: 触摸模式 0\n");
        printf("1: 触摸模式 1\n");
        printf("2: 不启用触摸\n");
        printf("请选择: ");
        scanf("%d", &choice);
        if (choice >= 0 && choice <= 1) {
            if (touch_init(choice))
                printf("触摸初始化成功 (模式 %d)\n", choice);
            else
                printf("触摸初始化失败\n");
        } else {
            printf("跳过触摸初始化\n");
        }
    }

    ~c_driver()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }

    bool is_ready() const
    {
        return fd >= 0;
    }

    bool initialize(pid_t pid)
    {
        if (pid <= 0)
            return false;
        this->pid = pid;
        return true;
    }

    bool read(uintptr_t addr, void *buffer, size_t size)
    {
        request req = {};
        if (fd < 0 || pid <= 0)
            return false;

        addr &= 0xFFFFFFFFFFFF;
        req.pid = pid;
        req.addr = addr;
        req.buffer = buffer;
        req.size = size;

        if (ioctl(fd, READ_MEM, &req) != 0)
        {
            return false;
        }
        return true;
    }

    template <typename T>
    T read(uintptr_t addr)
    {
        T res;
        if (read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    bool read_v2(uintptr_t addr, void *buffer, size_t size)
    {
        request req = {};
        if (fd < 0 || pid <= 0)
            return false;

        addr &= 0xFFFFFFFFFFFF;
        req.pid = pid;
        req.addr = addr;
        req.buffer = buffer;
        req.size = size;

        if (ioctl(fd, READ_MEM_V2, &req) != 0)
        {
            return false;
        }
        return true;
    }

    template <typename T>
    T read_v2(uintptr_t addr)
    {
        T res;
        if (read_v2(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    bool write(uintptr_t addr, void *buffer, size_t size)
    {
        request req = {};
        if (fd < 0 || pid <= 0)
            return false;

        addr &= 0xFFFFFFFFFFFF;
        req.pid = this->pid;
        req.addr = addr;
        req.buffer = buffer;
        req.size = size;
        
        if (ioctl(fd, WRITE_MEM, &req) != 0)
        {
            return false;
        }
        return true;
    }

    template <typename T>
    T write(uintptr_t addr)
    {
        T res;
        if (write(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    pid_t get_name_pid(const char *name)
    {
        request req = {};
        char buf[0x100];
        if (fd < 0 || !name)
            return -1;

        snprintf(buf, sizeof(buf), "%s", name);
        req.pid = 0;
        req.buffer = buf;

        if(ioctl(fd, GET_PID, &req) != 0)
        {
            return -1;
        }
        return req.pid;
    }

    uintptr_t get_module_base(const char *name)
    {
        request req = {};
        char buf[0x100];
        if (fd < 0 || pid <= 0 || !name)
            return 0;

        snprintf(buf, sizeof(buf), "%s", name);
        req.pid = this->pid;
        req.addr = 0;
        req.buffer = buf;

        if (ioctl(fd, MODULE_BASE, &req) != 0)
        {
            return 0;
        }
        return req.addr;
    }
    
    uintptr_t get_module_bss(const char *name)
    {
        request req = {};
        char buf[0x100];
        if (fd < 0 || pid <= 0 || !name)
            return 0;

        snprintf(buf, sizeof(buf), "%s", name);
        req.pid = this->pid;
        req.addr = 0;
        req.buffer = buf;

        if (ioctl(fd, MODULE_BSS, &req) != 0)
        {
            return 0;
        }
        return req.addr;
    }

    bool touch_init(int touch_mode)
    {
        touch_event_base teb = {};
        if (fd < 0)
            return false;
        if (touch_mode > 1 || touch_mode < 0) {
            printf("不支持的触摸模式\n");
            return false;
        }
        teb.slot = touch_mode;
        if (ioctl(fd, TOUCH_INIT, &teb) != 0)
        {
            if (errno == EALREADY) {
                printf("触摸已开启，当前模式: %d\n", teb.slot);
                return true;
            }
            return false;
        }
        return true;
    }

    bool touch_down(int slot, int x, int y)
    {
        touch_event_base teb = {};
        if (fd < 0 || slot < 0)
            return false;

        teb.slot = slot;
        teb.x = x;
        teb.y = y;
        
        if (ioctl(fd, TOUCH_DOWN, &teb) != 0)
        {
            return false;
        }
        return true;
    }

    bool touch_up(int slot)
    {
        touch_event_base teb = {};
        if (fd < 0 || slot < 0)
            return false;

        teb.slot = slot;
        teb.x = 0;
        teb.y = 0;

        if (ioctl(fd, TOUCH_UP, &teb) != 0)
        {
            return false;
        }
        return true;
    }

    bool gyro_init(int method)
    {
        int current_method = method;
        if (fd < 0)
            return false;
        if (method < 0 || method > 1) {
            printf("不支持的陀螺仪模式\n");
            return false;
        }
        if (ioctl(fd, GYRO_INIT, &current_method) != 0)
        {
            if (errno == EALREADY) {
                printf("陀螺仪已开启，当前模式: %d\n", current_method);
                return true;
            }
            return false;
        }
        return true;
    }

    bool gyro_modify(float x, float y)
    {
        gyro_config config = {};
        if (fd < 0)
            return false;

        config.enable = 1;
        memcpy(&config.x, &x, sizeof(uint32_t));
        memcpy(&config.y, &y, sizeof(uint32_t));

        if (ioctl(fd, GYRO_CONFIG, &config) != 0)
        {
            return false;
        }
        
        return true;
    }

    bool gyro_disable()
    {
        gyro_config config = {};
        if (fd < 0)
            return false;

        config.enable = 0;
        config.x = 0;
        config.y = 0;

        if (ioctl(fd, GYRO_CONFIG, &config) != 0)
        {
            return false;
        }
        return true;
    }

    bool bp_check_inited()
    {
        if (fd < 0)
            return false;
        return ioctl(fd, BP_CHECK_INITED, 0) == 0;
    }

    bool bp_init_driver()
    {
        if (fd < 0)
            return false;
        return ioctl(fd, BP_INIT_CMD, 0) == 0;
    }

    int bp_get_num_brps()
    {
        if (fd < 0)
            return -1;
        return ioctl(fd, BP_GET_NUM_BRPS, 0);
    }

    int bp_get_num_wrps()
    {
        if (fd < 0)
            return -1;
        return ioctl(fd, BP_GET_NUM_WRPS, 0);
    }

    uint64_t bp_inst(pid_t target_pid, uint64_t addr, uint32_t bp_len,
                     uint32_t bp_type, uint64_t reg_modify_mask = 0,
                     uint64_t fp_reg_modify_mask = 0,
                     struct bp_user_pt_regs *regs_to_set = nullptr,
                     uint32_t flags = 0)
    {
        struct bp_inst_args args = {};
        if (fd < 0)
            return 0;

        args.pid = target_pid;
        args.addr = addr;
        args.bp_len = bp_len;
        args.bp_type = bp_type;
        args.reg_modify_mask = reg_modify_mask;
        args.fp_reg_modify_mask = fp_reg_modify_mask;
        args.regs_to_set_ptr = (uint64_t)regs_to_set;
        args.flags = flags;

        if (ioctl(fd, BP_INST, &args) != 0)
            return 0;

        return args.addr;
    }

    uint64_t bp_inst(pid_t target_pid, uint64_t addr, uint32_t bp_len,
                     uint32_t bp_type, bp_reg_batch &batch,
                     uint32_t flags = 0)
    {
        return bp_inst(target_pid, addr, bp_len, bp_type,
                       batch.reg_mask, batch.fp_mask,
                       batch.empty() ? nullptr : &batch.regs, flags);
    }

    bool bp_uninst(uint64_t handle)
    {
        if (fd < 0 || !handle)
            return false;
        return ioctl(fd, BP_UNINST, &handle) == 0;
    }

    bool bp_suspend(uint64_t handle)
    {
        if (fd < 0 || !handle)
            return false;
        return ioctl(fd, BP_SUSPEND, &handle) == 0;
    }

    bool bp_resume(uint64_t handle)
    {
        if (fd < 0 || !handle)
            return false;
        return ioctl(fd, BP_RESUME, &handle) == 0;
    }

    bool bp_get_hit_count(uint64_t handle, uint64_t *total_count,
                          uint64_t *item_arr_count)
    {
        struct bp_get_hit_count_arg arg = {};
        if (fd < 0 || !handle)
            return false;

        arg.handle = handle;
        if (ioctl(fd, BP_GET_HIT_COUNT, &arg) != 0)
            return false;

        if (total_count)
            *total_count = arg.hit_total_count;
        if (item_arr_count)
            *item_arr_count = arg.hit_item_arr_count;
        return true;
    }

    bool bp_modify(uint64_t handle, uint64_t reg_modify_mask,
                   uint64_t fp_reg_modify_mask,
                   struct bp_user_pt_regs *regs_to_set)
    {
        struct bp_modify_args args = {};
        if (fd < 0 || !handle)
            return false;

        args.handle = handle;
        args.reg_modify_mask = reg_modify_mask;
        args.fp_reg_modify_mask = fp_reg_modify_mask;
        args.regs_to_set_ptr = (uint64_t)regs_to_set;

        return ioctl(fd, BP_MODIFY, &args) == 0;
    }

    size_t bp_get_hit_items(uint64_t handle, struct bp_hit_item *buffer,
                            size_t max_bytes)
    {
        struct bp_get_hit_items_args args = {};
        if (fd < 0 || !handle || !buffer || max_bytes == 0)
            return 0;

        args.handle = handle;
        args.user_buffer_ptr = (uint64_t)buffer;
        args.max_bytes = max_bytes;
        args.items_copied = 0;

        if (ioctl(fd, BP_GET_HIT_ITEMS, &args) != 0)
            return 0;

        return (size_t)args.items_copied;
    }

    std::vector<bp_hit_item> bp_get_hits(uint64_t handle, size_t max_items = 100)
    {
        std::vector<bp_hit_item> result;
        if (fd < 0 || !handle || max_items == 0)
            return result;

        uint64_t total = 0, arr_count = 0;
        if (!bp_get_hit_count(handle, &total, &arr_count) || arr_count == 0)
            return result;

        size_t count = (arr_count < max_items) ? (size_t)arr_count : max_items;
        result.resize(count);
        size_t copied = bp_get_hit_items(handle, result.data(), count * sizeof(bp_hit_item));
        result.resize(copied);
        return result;
    }

    bool bp_set_reg(uint64_t handle, int reg_idx, uint64_t value)
    {
        if (reg_idx < 0 || reg_idx > 30)
            return false;
        struct bp_user_pt_regs regs = {};
        regs.regs[reg_idx] = value;
        return bp_modify(handle, 1ULL << reg_idx, 0, &regs);
    }

    bool bp_set_regs(uint64_t handle, std::initializer_list<std::pair<int, uint64_t>> regs)
    {
        struct bp_user_pt_regs pt_regs = {};
        uint64_t mask = 0;
        for (const auto &r : regs) {
            if (r.first < 0 || r.first > 30)
                continue;
            pt_regs.regs[r.first] = r.second;
            mask |= 1ULL << r.first;
        }
        if (mask == 0)
            return false;
        return bp_modify(handle, mask, 0, &pt_regs);
    }

    static uint64_t get_xregs(const bp_hit_item &hit, int reg_idx)
    {
        if (reg_idx < 0 || reg_idx > 30)
            return 0;
        return hit.regs_info.regs[reg_idx];
    }

    static float get_vregs_float(const bp_hit_item &hit, int vreg_idx)
    {
        float val = 0;
        if (vreg_idx < 0 || vreg_idx > 31)
            return val;
        memcpy(&val, &hit.regs_info.vregs[vreg_idx], sizeof(float));
        return val;
    }

    static double get_vregs_double(const bp_hit_item &hit, int vreg_idx)
    {
        double val = 0;
        if (vreg_idx < 0 || vreg_idx > 31)
            return val;
        memcpy(&val, &hit.regs_info.vregs[vreg_idx], sizeof(double));
        return val;
    }

    static uint64_t get_pc(const bp_hit_item &hit)
    {
        return hit.regs_info.pc;
    }

    static uint64_t get_pstate(const bp_hit_item &hit)
    {
        return hit.regs_info.pstate;
    }

    bool bp_set_vreg_float(uint64_t handle, int vreg_idx, float value)
    {
        if (vreg_idx < 0 || vreg_idx > 31)
            return false;
        struct bp_user_pt_regs regs = {};
        __uint128_t v = 0;
        memcpy(&v, &value, sizeof(float));
        regs.vregs[vreg_idx] = v;
        return bp_modify(handle, 0, 1ULL << vreg_idx, &regs);
    }

    bool bp_set_vreg_double(uint64_t handle, int vreg_idx, double value)
    {
        if (vreg_idx < 0 || vreg_idx > 31)
            return false;
        struct bp_user_pt_regs regs = {};
        __uint128_t v = 0;
        memcpy(&v, &value, sizeof(double));
        regs.vregs[vreg_idx] = v;
        return bp_modify(handle, 0, 1ULL << vreg_idx, &regs);
    }

    bool bp_set_vregs_float(uint64_t handle, std::initializer_list<std::pair<int, float>> vregs)
    {
        struct bp_user_pt_regs pt_regs = {};
        uint64_t mask = 0;
        for (const auto &r : vregs) {
            if (r.first < 0 || r.first > 31)
                continue;
            __uint128_t v = 0;
            memcpy(&v, &r.second, sizeof(float));
            pt_regs.vregs[r.first] = v;
            mask |= 1ULL << r.first;
        }
        if (mask == 0)
            return false;
        return bp_modify(handle, 0, mask, &pt_regs);
    }

    bool bp_set_vregs_double(uint64_t handle, std::initializer_list<std::pair<int, double>> vregs)
    {
        struct bp_user_pt_regs pt_regs = {};
        uint64_t mask = 0;
        for (const auto &r : vregs) {
            if (r.first < 0 || r.first > 31)
                continue;
            __uint128_t v = 0;
            memcpy(&v, &r.second, sizeof(double));
            pt_regs.vregs[r.first] = v;
            mask |= 1ULL << r.first;
        }
        if (mask == 0)
            return false;
        return bp_modify(handle, 0, mask, &pt_regs);
    }

    bool bp_set_pc(uint64_t handle, uint64_t pc)
    {
        struct bp_user_pt_regs regs = {};
        regs.pc = pc;
        return bp_modify(handle, REG_MODIFY_PC, 0, &regs);
    }

    bool bp_set_pstate(uint64_t handle, uint64_t pstate)
    {
        struct bp_user_pt_regs regs = {};
        regs.pstate = pstate;
        return bp_modify(handle, REG_MODIFY_PSTATE, 0, &regs);
    }

    bool bp_apply(uint64_t handle, bp_reg_batch &batch)
    {
        if (batch.reg_mask == 0 && batch.fp_mask == 0)
            return false;
        return bp_modify(handle, batch.reg_mask, batch.fp_mask, &batch.regs);
    }

};
