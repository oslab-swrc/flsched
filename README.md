# flsched
Feather-Like Scheduler (FLSCHED) is a process scheduler focused on the manycore environment.
This repository contains the implementation of FLSCHED for the Intel Xeon-phi manycore system.

FLSCHED is licensed under the GPLv2 license.

## What is FLSCHED ##

The prevalence of manycore processors imposes new challenges in scheduler design.

First, schedulers should be able to handle the unprecedented high degree of parallelism.
When the CFS scheduler was introduced, quad-core servers were dominant in data centers.
Now, 32-core servers are standard in data centers.
Moreover, servers with more than 100 cores are becoming popular.
Under such a high degree of parallelism, a small sequential part in a system can break the performance and scalability of an application.

Second, the cost of context switching keeps increasing as the amount of context, which needs to be saved and restored, increasing.
In Intel architectures, the width of SIMD register file has increased from 128-bits to 256-bits and now to 512-bits in XMM, YMM, and AVX, respectively.
This problem becomes exaggerated when the limited memory bandwidth is shared among many CPU cores with small cache such as Xeon Phi processors.

FLSCHED is designed for manycore accelerators like Xeon Phi.
We adopt a lockless design to keep FLSCHED from becoming a sequential bottleneck.
This is particularly critical for manycore accelerators, which have a large number of CPU cores.

FLSCHED is also designed for minimizing the number of context switches.
Because a Xeon Phi processor has 2× larger vector registers than Xeon processors (i.e., 32 512-bit registers for Xeon Phi
and 16 512-bits registers for Xeon processor), and its per-core memory bandwidth and cache size are smaller than a Xeon processor, its overhead of context switching is higher than a Xeon processor.
Thus, it is critical to minimize the number of context switching as many as possible.
Finally, FLSCHED is tailored to throughput-oriented workloads, which are dominant in manycore accelerators.

## Getting Started ##

This section will be released after the paper is published.




