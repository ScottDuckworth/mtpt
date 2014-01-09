/**
 * @file
 * @author Scott Duckworth <sduckwo@clemson.edu>
 * @brief  Multi-Threaded Path Traversal
 *
 * @section LICENSE
 * Copyright (c) 2014, Clemson University
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of Clemson University nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef MTPT_H
#define MTPT_H

#include <sys/stat.h>
#include <sys/types.h>

/**
 * Assign each non-directory to a task.  The default is to process each
 * non-directory in the same task as the directory itself.
 */
#define MTPT_CONFIG_FILE_TASKS 1

/**
 * The type expected for the callback methods.
 */
typedef int (*mtpt_method_t)(void *arg, const char *path,
                              const struct stat *st);

/**
 * Traverses the file system starting at the given path using multiple
 * threads.  Blocks until the traversal is complete.
 *
 * This function is thread-safe.
 *
 * @param nthreads
 * Number of threads.
 *
 * @param config
 * Bitwise OR of MTPT_CONFIG_* options.
 *
 * @param dir_enter_method
 * The method to call when entering a directory.  The method must return
 * non-zero if the directory should be traversed.  Can be NULL.
 *
 * @param dir_exit_method
 * The method to call when leaving a directory.  The return value is ignored.
 * Can be NULL.
 *
 * @param file_method
 * The method to call for each file.  The return value is ignored.  Can be
 * NULL.
 *
 * @param error_method
 * The method to call for each error.  errno will be set to the appropriate
 * error.  The return value is ignored.  Can be NULL.
 *
 * @param arg
 * The argument to pass to the first parameter of the methods.
 *
 * @return 0 if successful, -1 if there was an error and sets errno
 */
int mtpt(size_t nthreads, int config, const char *path,
         mtpt_method_t dir_enter_method, mtpt_method_t dir_exit_method,
         mtpt_method_t file_method, mtpt_method_t error_method, void *arg);

#endif // MTPT_H
