#include "vita_host.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <pthread.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

// Newlib heap for the host side (RDRAM is a separate memblock, see librecomp recomp.cpp).
extern "C" {
    int _newlib_heap_size_user = 192 * 1024 * 1024;
    unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

// std::thread passes no attributes and VitaSDK's pthread then gives a 32 KB stack, too small for recompiled game code.
// Linked with -Wl,--wrap=pthread_create: attribute-less threads get the desktop-like size below.
static constexpr size_t kDefaultThreadStack = 1 * 1024 * 1024;

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    if (attr != nullptr) {
        return __real_pthread_create(thread, attr, start, arg);
    }
    pthread_attr_t sized;
    pthread_attr_init(&sized);
    pthread_attr_setstacksize(&sized, kDefaultThreadStack);
    const int ret = __real_pthread_create(thread, &sized, start, arg);
    pthread_attr_destroy(&sized);
    return ret;
}

namespace rs64::vita {
    std::filesystem::path data_dir() {
        return "ux0:data/RogueSquadronVita";
    }

    void init_paths_and_log() {
        sceIoMkdir("ux0:data", 0777);
        sceIoMkdir("ux0:data/RogueSquadronVita", 0777);
        const std::string log_path = (data_dir() / "log.txt").string();
        freopen(log_path.c_str(), "w", stderr);
        freopen(log_path.c_str(), "a", stdout);
        setvbuf(stderr, nullptr, _IONBF, 0);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        std::error_code ec;
        std::filesystem::current_path(data_dir(), ec);
        if (ec) {
            fprintf(stderr, "[vita] chdir to %s failed: %s\n", data_dir().c_str(), ec.message().c_str());
        }
    }

    void sync_log() {
        fflush(stderr);
        fflush(stdout);
        sceIoSync("ux0:", 0);
    }

    void load_env_file() {
        // Vita defaults (the env file below can override them).
        // No per-task parse snapshot: copying 8 MB takes ~35 ms on the Vita, about as long as the parse it would overlap.
        setenv("ROGUESQ_PARSE_SNAPSHOT", "0", 0);
        // Terrain tiles subdivided once instead of twice: 44% fewer terrain triangles for slightly coarser slopes.
        setenv("ROGUESQ_F5_TERRAIN_SUB", "1", 0);
        std::ifstream in(data_dir() / "roguesq_env.txt");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
        }
    }

    unsigned long long thread_cpu_us() {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) {
            return 0;
        }
        return info.runClocks;
    }

    int thread_affinity() {
        return sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    }

    void set_max_clocks() {
        scePowerSetArmClockFrequency(444);
        scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(222);
        scePowerSetGpuXbarClockFrequency(166);
        fprintf(stderr, "[vita] clocks: arm=%d bus=%d gpu=%d xbar=%d MHz\n",
            scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    }
}
