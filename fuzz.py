#!/usr/bin/python3

import itertools
import os
import random
import sys
import enum

def run_or_exit(cmd):
    print(f"++ {cmd}")
    if os.system(cmd) != 0:
        raise ValueError("command returned non-zero exit code")

def get_random_divisor(num_procs):
    divisors = [n for n in range(2, num_procs) if num_procs % n == 0]
    if len(divisors) == 0:
        return 0
    else:
        return random.choice(divisors)

def get_levels(num_procs):
    levels = [num_procs]
    for _ in range(random.randint(0, 4)):
        if (n := get_random_divisor(levels[-1])) == 0:
            break
        else:
            levels.append(n)

    return levels[1:]

def fuzz_merge_sort(target, fixed_args, min_procs, max_procs, repeat):
    for _ in repeat:
        random_args = ["--sample-chars", "--sample-indexed", "--sample-random", "--rquick-lcp",
                       "--lcp-compression", "--prefix-compression", "--prefix-doubling",
                       "--grid-bloomfilter"]

        nargs = random.randint(0, len(random_args))
        choice = random.sample(random_args, k=nargs)
        procs = random.randint(min_procs, max_procs)
        levels = get_levels(procs)

        num_strings = random.randint(0, 10000)
        len_strings = random.randint(1, 500)
        dn_ratio = random.random()
        sampling_factor = random.randint(1, 10)

        run_or_exit(f"mpirun -n {procs} --oversubscribe"
              f" target/{target}/distributed_sorter -v -V -i 3"
              f" --num-strings {num_strings} --len-strings {len_strings}"
              f" --DN-ratio {dn_ratio} --sampling-factor {sampling_factor}"
              f" {' '.join(fixed_args)} {' '.join(choice)} "
              f" {' '.join(map(str, levels))}")

def fuzz_space_efficient_sort(target, fixed_args, min_procs, max_procs, repeat):
    class StringGenerator(enum.Enum):
        DNRatio = (1, 0)
        Suffix = (0, 0)
        Window = (0, 1)
        DifferenceCover = (0, 2)

    for _ in repeat:
        random_args = ["--sample-chars", "--sample-indexed", "--sample-random",
                       "--quantile-chars", "--quantile-indexed", "--quantile-random",
                       "--rquick-lcp", "--lcp-compression", "--prefix-compression",
                       "--prefix-doubling", "--grid-bloomfilter", "--shuffle"]

        nargs = random.randint(0, len(random_args))
        choice = random.sample(random_args, k=nargs)
        procs = random.randint(min_procs, max_procs)
        levels = get_levels(procs)

        generator = random.choice(list(StringGenerator))
        num_chars = random.randint(1, 50000) \
            if generator == StringGenerator.Suffix \
            else random.randint(1, 500000)
        combined_gnerator, string_generator = generator.value
        char_generator = 0
        len_strings = random.randint(1, 5000)
        num_strings = random.randint(1, 5000)
        dn_ratio = random.random()
        step = random.randint(1, 16)
        dc = random.choice([3, 7, 13, 21, 31, 32, 64, 512, 1024, 2048, 4096, 8192])

        quantile_size = random.randint(20, 200000)
        sampling_factor = random.randint(1, 10)
        quantile_factor = random.randint(1, 10)

        run_or_exit(f"mpirun -n {procs} --oversubscribe"
            f" target/{target}/space_efficient_sorter -v -V -i 3"
            f" --sampling-factor {sampling_factor} --use-quantile-sampler"
            f" --quantile-factor {quantile_factor} --quantile-size {quantile_size}KiB"
            f" --combined-generator {combined_gnerator} --string-generator {string_generator}"
            f" --char-generator {char_generator} --num-chars {num_chars} --len-strings {len_strings}"
            f" --num-strings {num_strings} --dn-ratio {dn_ratio} --step {step} --difference-cover {dc}"
            f" {' '.join(fixed_args)} {' '.join(choice)} "
            f" {' '.join(map(str, levels))}")

def fuzz_all(target, fixed_args, min_procs, max_procs, repeat):
    for _ in repeat:
        if random.randint(0, 1) == 0:
            fuzz_merge_sort(target, fixed_args, min_procs, max_procs, [0])
        else:
            fuzz_space_efficient_sort(target, fixed_args, min_procs, max_procs, [0])

def build(bin, target):
    match bin:
        case "all":
            run_or_exit(f"make -C target/{target} -j5 distributed_sorter")
            run_or_exit(f"make -C target/{target} -j5 space_efficient_sorter")
        case "merge-sort":
            run_or_exit(f"make -C target/{target} -j5 distributed_sorter")
        case "space-efficient":
            run_or_exit(f"make -C target/{target} -j5 space_efficient_sorter")
        case _:
            raise ValueError("invalid binary")

if __name__ == "__main__":
    bin = sys.argv[2]
    target = sys.argv[3]

    match sys.argv[1]:
        case "fuzz":
            build(bin, target)

            min_procs, max_procs = int(sys.argv[4]), int(sys.argv[5])
            match bin:
                case "merge-sort":
                    fuzz_merge_sort(target, sys.argv[6:], min_procs, max_procs, itertools.repeat(0))
                case "space-efficient":
                    fuzz_space_efficient_sort(target, sys.argv[6:], min_procs, max_procs, itertools.repeat(0))
                case "all":
                    fuzz_all(target, sys.argv[6:], min_procs, max_procs, itertools.repeat(0))

        case "build":
            build(bin, target)

        case "run":
            nprocs = sys.argv[4]
            build(bin, target)

            args = sys.argv[5:]
            executable = "distributed_sorter" if bin == "merge-sort" else "space_efficient_sorter" # todo
            run_or_exit(f"mpirun -n {nprocs} --oversubscribe target/{target}/{executable} {' '.join(args)}")

        case "debug":
            nprocs = sys.argv[4]
            build(bin, target)

            args = sys.argv[5:]
            executable = "distributed_sorter" if bin == "merge-sort" else "space_efficient_sorter" # todo
            run_or_exit(f"mpirun -n {nprocs} --oversubscribe kitty gdb -ex run --args target/{target}/{executable} {' '.join(args)}")

        case _:
            raise ValueError("invalid command/binary")
