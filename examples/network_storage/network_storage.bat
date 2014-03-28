@echo off

rem #
rem # Run this script with a single argument N = number of tasks to spawn.
rem # All tasks are spawned on the local machine, this should not be used 
rem # for benchmarking, only testing.
rem #

rem Path=D:\build\hpx\Debug\lib\hpx;C:\Boost\lib;c:\Program Files\HDF5_1_8_cmake-vs11\bin
rem cd /d d:\build\hpx

rem Get N-1 from argument
set /a N=%1-1

rem use "start /B" to suppress new window per task 

echo "Starting %1 instances as part of HPX job"
FOR /l %%x IN (0, 1, %N%) DO (
  echo start /B D:\build\hpx\Debug\bin\network_storage.exe -l%1 -%%x --hpx:run-hpx-main -Ihpx.parcel.tcp.enable=1 -Ihpx.parcel.async_serialization=1 --hpx:threads=4 -Ihpx.agas.max_pending_refcnt_requests=0 --localMB=512 --transferKB=1024 --iterations=5  
  start         D:\build\hpx\Debug\bin\network_storage.exe -l%1 -%%x --hpx:run-hpx-main -Ihpx.parcel.tcp.enable=1 -Ihpx.parcel.async_serialization=1 --hpx:threads=4 -Ihpx.agas.max_pending_refcnt_requests=0 --localMB=512 --transferKB=1024 --iterations=1
)
 
rem 2097152 65536
rem
rem --hpx:debug-clp  
rem --hpx:list-component-types
rem --hpx:threads=2 
rem  -Ihpx.parcel.async_serialization=0
rem  -Ihpx.parcel.tcp.enable=1  
rem  -Ihpx.parcel.bootstrap=mpi 
rem  -Ihpx.agas.max_pending_refcnt_requests=0
rem  -Ihpx.threadpools.parcel_pool_size=2
rem  -Ihpx.parcel.async_serialization=1 
