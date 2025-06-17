#
# Copyright (c) 2025, Carnegie Mellon University.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of the University nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
# HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
# WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

#
# CMakeLists.txt  cmake for mvpnet
# 14-Apr-2025  chuck@ece.cmu.edu
#

find_package(Threads REQUIRED)
find_package(MPI MODULE REQUIRED)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mvp_mlog.h
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/mkmloghdr.sh > 
                                     ${CMAKE_CURRENT_BINARY_DIR}/mvp_mlog.h
    DEPENDS mkmloghdr.sh)

add_executable(mvpnet fbufmgr.c fdio_thread.c mlog.c mpi_thread.c
               mvp_mlog.h mvp_queuing.c mvp_stats.c mvpnet.c pktfmt.c
               qemucli.c)
target_include_directories(mvpnet PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
                                          ${CMAKE_CURRENT_SOURCE_DIR})
if(MVP_MINLOG)
    message(STATUS "note: disable all mlog calls lower than MLOG_WARN")
    target_compile_options(mvpnet PRIVATE -DMLOG_MINPRIORITY=MLOG_WARN)
endif()
target_link_libraries(mvpnet utilfns MPI::MPI_C Threads::Threads)

# "make install" rule
install (TARGETS mvpnet RUNTIME DESTINATION bin)
