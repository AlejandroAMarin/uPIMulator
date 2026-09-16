import argparse
import os
import shutil
import subprocess


def generator():
    # Ninja keeps only object files in <target>.dir/, which is what the linker
    # wants, but it is not always installed. Unix Makefiles also works because
    # the linker filters that directory by suffix.
    return "Ninja" if shutil.which("ninja") else "Unix Makefiles"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--num_dpus", type=int, default=1)
    parser.add_argument("--num_tasklets", type=int, default=1)
    parser.add_argument(
        "--dpu_cc",
        type=str,
        default="dpu-upmem-dpurte-clang",
        help="DPU compiler from the installed UPMEM SDK",
    )
    args = parser.parse_args()

    benchmark_dir_path = os.path.dirname(os.path.abspath(__file__))

    build_dir_path = os.path.join(benchmark_dir_path, "build")

    if os.path.exists(build_dir_path):
        shutil.rmtree(build_dir_path)
    os.makedirs(build_dir_path)

    subprocess.run(
        [
            "cmake",
            f"-DNR_DPUS={args.num_dpus}",
            f"-DNR_TASKLETS={args.num_tasklets}",
            f"-DDPU_CC={args.dpu_cc}",
            "-S",
            benchmark_dir_path,
            "-B",
            build_dir_path,
            "-G",
            generator(),
        ],
        check=True,
    )
    subprocess.run(["cmake", "--build", build_dir_path, "--parallel"], check=True)
