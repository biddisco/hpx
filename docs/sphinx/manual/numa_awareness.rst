..
    Copyright (C) 2019 John Biddiscombe

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

=============
NUMA Awareness
=============

.. _numa_domains:

NUMA Domains
==============

When running MPI based code on machines with multiple sockets, it is common
practice to assign multiple ranks to run on the same node. The reason for
this is that confining one rank to a socket (and sometimes one rank per core)
can improve performance as cross numa traffic is reduced.
Combining one MPI rank per socket with OpenMP threading is becoming the
standard approach to writing HPC codes as it can minimize cross numa traffic
improves cache coherency and add multithreading support.
In this section we see how to achieve the same kind of isolation and effects
using HPX resource partitioner, thread pools, scheduler and numa allocator.

.. _thread_pools:

Thread Pools
==============
On startup, HPX by default creates a thread pool than spans all the resource
available (or requested via the command line), the resource_partitioner can
be used to customize how thread pools are setup.
