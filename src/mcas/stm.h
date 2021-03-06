/******************************************************************************
 * stm.h
 *
 * Interface definitions for software transactional memory (STM).
 *
 * Copyright (c) 2002-2003, K A Fraser

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
    * notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
    * copyright notice, this list of conditions and the following
    * disclaimer in the documentation and/or other materials provided
    * with the distribution.  Neither the name of the Keir Fraser
    * nor the names of its contributors may be used to endorse or
    * promote products derived from this software without specific
    * prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ptst.h"
#include <setjmp.h>

typedef struct stm_st stm;
typedef struct stm_blk_st stm_blk;
typedef struct stm_tx_st stm_tx;

stm *new_stm(ptst_t *ptst, int blk_size);
void free_stm(ptst_t *ptst, stm *mem);

stm_blk *new_stm_blk(ptst_t *ptst, stm *mem);
void free_stm_blk(ptst_t *ptst, stm *mem, stm_blk *b);
void *init_stm_blk(ptst_t *ptst, stm *mem, stm_blk *b);
int sizeof_stm_blk(ptst_t *ptst, stm *mem, stm_blk *b);

stm_tx *new_stm_tx(ptst_t *ptst, stm *mem, sigjmp_buf *penv);
bool_t commit_stm_tx(ptst_t *ptst, stm_tx *t);
bool_t validate_stm_tx(ptst_t *ptst, stm_tx *t);
/* NB. Must still call commit after abort, but it's guaranteed to fail. */
void abort_stm_tx(ptst_t *ptst, stm_tx *t);

void *read_stm_blk(ptst_t *ptst, stm_tx *t, stm_blk *b);
void *write_stm_blk(ptst_t *ptst, stm_tx *t, stm_blk *b);

void remove_from_tx(ptst_t *ptst, stm_tx *t, stm_blk *b);

void _init_stm_subsystem(int pad_data);

#define new_stm_tx(_tx, _ptst, _mem)                \
    do {                                            \
        sigjmp_buf env;                             \
        sigsetjmp(env, 1);                          \
        (_tx) = new_stm_tx((_ptst), (_mem), &env);  \
    } while ( 0 )
