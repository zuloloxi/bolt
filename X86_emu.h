/*
 *  _           _ _   
 * | |         | | |  
 * | |__   ___ | | |_ 
 * | '_ \ / _ \| | __|
 * | |_) | (_) | | |_ 
 * |_.__/ \___/|_|\__|
 *
 * Written by Dennis Yurichev <dennis(a)yurichev.com>, 2013
 *
 * This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivs 3.0 Unported License. 
 * To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/3.0/.
 *
 */

#pragma once

#include "memorycache.h"
#include "x86_disas.h"
#include "CONTEXT_utils.h"

typedef enum _Da_emulate_result
{
    DA_NOT_EMULATED,
    DA_EMULATED_OK,
    DA_EMULATED_CANNOT_READ_MEMORY,
    DA_EMULATED_CANNOT_WRITE_MEMORY,
    DA_EMULATED_NOT_SUPPORTED
} Da_emulate_result;

Da_emulate_result Da_emulate_MOV_op1_op2(Da *d, CONTEXT * ctx, MemoryCache *mem);
Da_emulate_result Da_emulate_Jcc (Da* d, bool cond, CONTEXT * ctx);
Da_emulate_result Da_emulate_CMOVcc (Da* d, bool cond, CONTEXT * ctx, MemoryCache *mem);
Da_emulate_result Da_emulate_SETcc (Da* d, bool cond, CONTEXT * ctx, MemoryCache *mem);
Da_emulate_result Da_emulate(Da* d, CONTEXT * ctx, MemoryCache *mem);
const char* Da_emulate_result_to_string(Da_emulate_result r);

/* vim: set expandtab ts=4 sw=4 : */