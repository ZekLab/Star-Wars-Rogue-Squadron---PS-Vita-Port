#pragma once

#include <filesystem>

namespace rs64::vita {
    // ux0:data/RogueSquadronVita — ROM, saves, configs, logs and roguesq_env.txt live here.
    std::filesystem::path data_dir();
    // Creates data_dir(), makes it the working directory and redirects stdout/stderr to data_dir()/log.txt.
    void init_paths_and_log();
    // setenv() for every NAME=VALUE line of data_dir()/roguesq_env.txt; '#' starts a comment line.
    void load_env_file();
    // Commits the log to the memory card; without it a console reset loses everything written since launch.
    void sync_log();
    // Max CPU/bus/GPU clocks (ARM 444 MHz).
    void set_max_clocks();
    // CPU time the calling thread has actually run, in microseconds (wall time minus preemption/waits).
    unsigned long long thread_cpu_us();
    // CPU affinity mask of the calling thread.
    int thread_affinity();
    // ROGUESQ_VG_BENCH=<capture dir under data_dir()>: runs the parser bench (vita_bench.cpp) before the game starts.
    void run_parser_bench();
}
