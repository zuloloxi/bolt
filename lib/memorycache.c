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

#include "memorycache.h"
#include "dmalloc.h"
#include "rbtree.h"
#include "stuff.h"
#include "mem_utils.h"
#include "oassert.h"
#include "fmt_utils.h"

MemoryCache* MC_MemoryCache_ctor(HANDLE PHDL, bool dont_read_from_quicksilver_places)
{
	MemoryCache* rt=DCALLOC(MemoryCache, 1, "MemoryCache");
	rt->PHDL=PHDL;
	rt->_cache=rbtree_create(true, "MemoryCache._cache", compare_size_t);
	rt->dont_read_from_quicksilver_places=dont_read_from_quicksilver_places;
	return rt;
};

#ifdef BOLT_DEBUG
MemoryCache* MC_MemoryCache_ctor_testing(BYTE *testing_memory, SIZE_T testing_memory_size)
{
	oassert ((testing_memory_size & (PAGE_SIZE-1))==0);
	MemoryCache* rt=DCALLOC(MemoryCache, 1, "MemoryCache");
	rt->_cache=rbtree_create(true, "MemoryCache._cache", compare_size_t);
	rt->testing=true;
	rt->testing_memory=testing_memory;
	rt->testing_memory_size=testing_memory_size;
	return rt;
};
#endif

void MC_MemoryCache_dtor(MemoryCache *mc, bool check_unflushed_elements)
{
	struct rbtree_node_t *i;

	if (check_unflushed_elements)
	{
		for (i=rbtree_minimum(mc->_cache); i!=NULL; i=rbtree_succ(i))
		{
			MemoryCacheElement *v=(MemoryCacheElement*)i->value;
			if (v->to_be_flushed)
			{
				printf ("%s(): there are still elements to be flushed!\n", __FUNCTION__);
				oassert(0);
			};
		};
	};

	rbtree_foreach(mc->_cache, NULL, NULL, dfree);
	rbtree_deinit(mc->_cache);
	DFREE (mc);
};

static void* key_copier(void *i)
{
	return i;
};

static void* value_copier(void *v)
{
	return DMEMDUP (v, sizeof(MemoryCacheElement), "MemoryCacheElement");
};

MemoryCache* MC_MemoryCache_copy_ctor (MemoryCache *mc)
{
	MemoryCache* rt;

	//L (2, __FUNCTION__"(): begin\n");

	rt=DCALLOC(MemoryCache, 1, "MemoryCache"); 
	rt->PHDL=mc->PHDL;
	rt->dont_read_from_quicksilver_places=mc->dont_read_from_quicksilver_places;
	rt->_cache=rbtree_create(true, "MemoryCache._cache", compare_size_t);
	rbtree_copy (mc->_cache, rt->_cache, key_copier, value_copier);

#ifdef BOLT_DEBUG
	rt->testing=mc->testing;
	rt->testing_memory=mc->testing_memory;
	rt->testing_memory_size=mc->testing_memory_size;
#endif
	return rt;
};

bool MC_LoadPageForAddress (MemoryCache *mc, address adr)
{
	address idx, rd_adr;
	SIZE_T bytes_read;
	MemoryCacheElement *t=NULL;

#ifndef _WIN64
	// as of win32
#define _ADR_SKIP 0x7FFE0000
	if (mc->dont_read_from_quicksilver_places && (adr>=_ADR_SKIP && adr<(_ADR_SKIP+PAGE_SIZE)))
	{
		// нужно всегда обламывать чтение этих мест - там, например, текущее системное время, 
		// от этого тестирование эмулятора CPU рандомно глючит, долго я искал эту багу :(

		// с другой стороны, эмулятор CPU вполне может нормально работать, хоть и с небольшими 
		// отклонениями по системному времени
		//L (2, __FUNCTION__ "(0x" PRI_ADR_HEX "): wouldn't read process memory\n");
		return false;
	};
#endif

	idx=adr>>LOG2_PAGE_SIZE;
	rd_adr=idx<<LOG2_PAGE_SIZE;
	t=DCALLOC(MemoryCacheElement, 1, "MemoryCacheElement");

#ifdef BOLT_DEBUG
	if (mc->testing)
	{
		if (rd_adr+PAGE_SIZE > mc->testing_memory_size)
			goto free_t_and_return_false;

		memcpy (t->block, mc->testing_memory+rd_adr, PAGE_SIZE);
		bytes_read=PAGE_SIZE;
	};
#endif

#ifdef BOLT_DEBUG
	if (mc->testing==false)
#endif
		if (ReadProcessMemory (mc->PHDL, (LPCVOID)rd_adr, t->block, PAGE_SIZE, &bytes_read)==false)
			goto free_t_and_return_false;

	oassert (bytes_read==PAGE_SIZE);
	rbtree_insert(mc->_cache, (void*)idx, t);
	return true;

free_t_and_return_false:
	DFREE(t);
	return false;
};

bool MC_ReadBuffer (MemoryCache *mc, address adr, SIZE_T size, BYTE* outbuf)
{
	SIZE_T i;
	// FIXME: это временное решение. и тормозное, конечно
	for (i=0; i<size; i++)
		if (MC_ReadByte (mc, adr+i, &outbuf[i])==false)
			return false;
	return true;
};

bool MC_WriteBuffer (MemoryCache *mc, address adr, SIZE_T size, BYTE* inbuf)
{
	SIZE_T i;
	// FIXME: это временное решение. и тормозное, конечно
	for (i=0; i<size; i++)
		if (MC_WriteByte (mc, adr+i, inbuf[i])==false)
			return false;
	return true;
};

BYTE* MC_find_page_ptr(MemoryCache *mc, address adr)
{
	address idx=adr>>LOG2_PAGE_SIZE;

	if (mc->last_ptr_idx_present && idx==mc->last_ptr_idx)
		return mc->last_ptr;
	else
	{
		MemoryCacheElement *tmp=(MemoryCacheElement*)rbtree_lookup(mc->_cache, (void*)idx);
		if (tmp==NULL)
		{
			if (MC_LoadPageForAddress (mc, adr)==false) // подгружаем блок если у нас его нету
				return NULL;
			tmp=(MemoryCacheElement*)rbtree_lookup(mc->_cache, (void*)idx);
			oassert (tmp!=NULL);
			mc->last_ptr=tmp->block;
			mc->last_ptr_idx=idx;
			mc->last_ptr_idx_present=true;
			return mc->last_ptr;
		}
		else
			return tmp->block;
	};
};

bool MC_ReadByte (MemoryCache *mc, address adr, BYTE * out)
{
	BYTE* p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;
	*out=p[adr&(PAGE_SIZE-1)];
	return true;
};

void MC_mark_as_to_be_flushed(MemoryCache *mc, address idx)
{
	MemoryCacheElement *m=(MemoryCacheElement*)rbtree_lookup(mc->_cache, (void*)idx);
	oassert (m!=NULL);
	m->to_be_flushed=true;
};

bool MC_WriteByte (MemoryCache *mc, address adr, BYTE val)
{
	address idx;
	BYTE *p;

	p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;
	idx=adr>>LOG2_PAGE_SIZE;

	p[adr&(PAGE_SIZE-1)]=val;
	MC_mark_as_to_be_flushed(mc, idx);
	return true;
};

bool MC_ReadWyde (MemoryCache *mc, address adr, WORD * out)
{
	BYTE *p;
	int adr_frac;

	p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;

	adr_frac=adr&(PAGE_SIZE-1);
	// if this WORD is right on joint of two pages...
	if (adr_frac>(PAGE_SIZE-sizeof(WORD)))
	{
		BYTE b1, b2;

		if (MC_ReadByte(mc, adr+1, &b1)==false)
			return false;
		if (MC_ReadByte(mc, adr, &b2)==false)
			return false;

		*out=(b1 << 8) | b2;
	}
	else
		*out=*(WORD*)(p+adr_frac);
	return true;
};

bool MC_WriteWyde (MemoryCache *mc, address adr, WORD val)
{
	address idx;
	BYTE *p;
	unsigned adr_frac;

	p=MC_find_page_ptr (mc, adr);
	idx=adr>>LOG2_PAGE_SIZE;

	if (p==NULL)
		return false;

	adr_frac=adr&(PAGE_SIZE-1);
	// а если этот WORD на границе двух страниц...
	if (adr_frac>(PAGE_SIZE-sizeof(WORD)))
	{
		if (MC_WriteByte (mc, adr+0, val&0xFF)==false)
			return false;
		if (MC_WriteByte (mc, adr+1, val>>8)==false)
			return false;
	}
	else
	{
		*(WORD*)(p+adr_frac)=val;
		MC_mark_as_to_be_flushed(mc, idx);
	};

	return true;
};

bool MC_ReadTetrabyte (MemoryCache *mc, address adr, DWORD * out)
{
	//L (2, __FUNCTION__ "(): adr=0x" PRI_ADR_HEX "\n", adr);

	BYTE *p;
	unsigned adr_frac;

	p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;

	adr_frac=adr&(PAGE_SIZE-1);
	// а если этот DWORD на границе двух страниц...
	if (adr_frac>(PAGE_SIZE-sizeof(DWORD)))
	{
		BYTE b1=0, b2=0, b3=0, b4=0; // TMCH
		bool read_OK=true;

		if (MC_ReadByte(mc, adr+3, &b1)==false)
			read_OK=false;
		if (MC_ReadByte(mc, adr+2, &b2)==false)
			read_OK=false;
		if (MC_ReadByte(mc, adr+1, &b3)==false)
			read_OK=false;
		if (MC_ReadByte(mc, adr+0, &b4)==false)
			read_OK=false;

		if (read_OK==false)
		{
			//L (2, __FUNCTION__ "(0x" PRI_ADR_HEX "): one of ReadByte() funcs failed\n", adr);
			return false;
		};

		*out=(b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
	}
	else
		*out=*(DWORD*)(p+adr_frac);
	return true;
};

bool MC_WriteTetrabyte (MemoryCache *mc, address adr, DWORD val)
{
	address idx;
	BYTE *p;
	unsigned adr_frac;

	p=MC_find_page_ptr (mc, adr);

	if (p==NULL)
		return false;

	idx=adr>>LOG2_PAGE_SIZE;

	adr_frac=adr&(PAGE_SIZE-1);
	// а если этот DWORD на границе двух страниц...
	if (adr_frac>(PAGE_SIZE-sizeof(DWORD)))
	{
		if (MC_WriteByte (mc, adr+0, val&0xFF)==false)
			return false;
		if (MC_WriteByte (mc, adr+1, (val>>8)&0xFF)==false)
			return false;
		if (MC_WriteByte (mc, adr+2, (val>>16)&0xFF)==false)
			return false;
		if (MC_WriteByte (mc, adr+3, (val>>24)&0xFF)==false)
			return false;
	}
	else
	{
		*(DWORD*)(p+adr_frac)=val;
		MC_mark_as_to_be_flushed(mc, idx);
	};

	return true;
};

bool MC_ReadOctabyte (MemoryCache *mc, address adr, DWORD64 * out)
{
	unsigned adr_frac;
	BYTE *p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;

	adr_frac=adr&(PAGE_SIZE-1);
	// а если этот DWORD64 на границе двух страниц...
	if (adr_frac>(PAGE_SIZE-sizeof(DWORD64)))
	{
		DWORD d1, d2;

		if (MC_ReadTetrabyte(mc, adr+4, &d1)==false)
			return false;
		if (MC_ReadTetrabyte(mc, adr+0, &d2)==false)
			return false;

		*out=((uint64_t)d1 << 32) | d2;
	}
	else
		*out=*(DWORD64*)(p+adr_frac);
	return true;
};

bool MC_WriteOctabyte (MemoryCache *mc, address adr, DWORD64 val)
{
	int adr_frac;
	address idx=adr>>LOG2_PAGE_SIZE;
	BYTE *p=MC_find_page_ptr (mc, adr);
	if (p==NULL)
		return false;

	adr_frac=adr&(PAGE_SIZE-1);
	// а если этот DWORD64 на границе двух страниц...
	if (adr_frac>(PAGE_SIZE-sizeof(DWORD64)))
	{
		if (MC_WriteTetrabyte (mc, adr+0, val&0xFFFFFFFF)==false)
			return false;
		if (MC_WriteTetrabyte (mc, adr+4, (val>>32)&0xFFFFFFFF)==false)
			return false;
	}
	else
	{
		*(DWORD64*)(p+adr_frac)=val;
		MC_mark_as_to_be_flushed(mc, idx);
	};

	return true;
};

bool MC_ReadREG (MemoryCache *mc, address a, REG * out)
{
#ifdef _WIN64
	return MC_ReadOctabyte (mc, a, (DWORD64*)out);
#else
	return MC_ReadTetrabyte (mc, a, (DWORD*)out);
#endif
};

bool MC_WriteREG (MemoryCache *mc, address a, REG val)
{
#ifdef _WIN64
	return MC_WriteOctabyte (mc, a, val);
#else
	return MC_WriteTetrabyte (mc, a, val);
#endif
};

void MC_Flush(MemoryCache *mc)
{
	struct rbtree_node_t *i;

	for (i=rbtree_minimum(mc->_cache); i!=NULL; i=rbtree_succ(i))
	{
		MemoryCacheElement *v=(MemoryCacheElement*)i->value;
		if (v->to_be_flushed)
		{
			address adr=((size_t)i->key) << LOG2_PAGE_SIZE;
#ifdef BOLT_DEBUG
			if (mc->testing)
			{
				oassert (adr+PAGE_SIZE < mc->testing_memory_size);
				memcpy (mc->testing_memory+adr, v->block, PAGE_SIZE);
			};
#endif       

#ifdef BOLT_DEBUG
			if (mc->testing==false)
#endif            
				if (WriteProcessMemory (mc->PHDL, (LPVOID)adr, v->block, PAGE_SIZE, NULL)==FALSE)
				{
					MEMORY_BASIC_INFORMATION info;
					int rt=VirtualQueryEx (mc->PHDL, (LPVOID)adr, &info, sizeof (MEMORY_BASIC_INFORMATION));
					oassert (rt==sizeof(MEMORY_BASIC_INFORMATION));
					//dump_MEMORY_BASIC_INFORMATION (&info);

					DWORD tmp, tmp2;
					if (VirtualProtectEx(mc->PHDL, info.BaseAddress, info.RegionSize, PAGE_READWRITE, &tmp)==FALSE)
						die ("%s(): first VirtualProtectEx() failed. exiting.\n", __func__);
					if (WriteProcessMemory (mc->PHDL, (LPVOID)adr, v->block, PAGE_SIZE, NULL)==FALSE)
						die ("%s(): second try WriteProcessMemory() also failed. exiting.\n", __func__);
					if (VirtualProtectEx(mc->PHDL, info.BaseAddress, info.RegionSize, tmp, &tmp2)==FALSE)
						die ("%s(): second VirtualProtectEx() failed. exiting.\n", __func__);
				};
			v->to_be_flushed=false;
		};
	};
};

void MC_dump_state(fds *s, MemoryCache *mc)
{
	rbtree_node *i;
	L_fds (s, "%s()\n", __FUNCTION__);

	for (i=rbtree_minimum(mc->_cache); i!=NULL; i=rbtree_succ(i))
	{
		MemoryCacheElement *v=(MemoryCacheElement*)i->value;
		L_fds (s, "adr=0x" PRI_ADR_HEX ", to_be_flushed=", (size_t)i->key, v->to_be_flushed);
	};
};

bool MC_CompareInternalStateWithMemory(MemoryCache *mc)
{
	BYTE* tmp=DMALLOC(BYTE, PAGE_SIZE, "tmp");
	bool rt=true;
	struct rbtree_node_t *i;
	int j;

	for (i=rbtree_minimum(mc->_cache); i!=NULL; i=rbtree_succ(i))
	{
		MemoryCacheElement *v=(MemoryCacheElement*)i->value;
		if (v->to_be_flushed)
		{
			address adr=((size_t)i->key) << LOG2_PAGE_SIZE;
			SIZE_T bytes_read;

#ifdef BOLT_DEBUG
			if (mc->testing)
			{
				oassert (adr+PAGE_SIZE < mc->testing_memory_size);
				memcpy (tmp, mc->testing_memory+adr, PAGE_SIZE);
			};
#endif

#ifdef BOLT_DEBUG
			if (mc->testing==false)
#endif
				if (ReadProcessMemory (mc->PHDL, (LPCVOID)adr, tmp, PAGE_SIZE, &bytes_read)==false)
					die ("%s(): can't read memory. fatal error. exiting\n", __FUNCTION__);

			oassert (bytes_read==PAGE_SIZE);

			if (memcmp (v->block, tmp, PAGE_SIZE)!=0)
			{
				rt=false;
				for (j=0; j<PAGE_SIZE; j++)
				{
					if (v->block[j]!=tmp[j])
					{
						L ("%s() bytes are different at adr 0x" PRI_ADR_HEX ": in cache: 0x%02X, in memory: 0x%02X\n",
								__FUNCTION__, adr+j, v->block[j], tmp[j]);
					};
				};
			};
		};
	};

	DFREE (tmp);
	return rt;
};

// FIXME: invent a good name for it!
static bool my_isprint (char a)
{
	if (a==0x0A)
		return true;

	if (a==0x0D)
		return true;

	if (a<0x20)
		return false;

	if (a>=0x7F)
		return false;

	return true;
};

// FIXME: slow
// can output something to out, but eventually return false
// in case of "\x00" returns true, meaning, empty string
// emptry strings like " " are OK too. I need another function for string validity checking!
bool MC_GetString (MemoryCache *mc, address adr, bool unicode, strbuf * out)
{
	int step, i;
	byte by, _out;
	int chars_read=0;

	if (unicode)
		step=2;
	else
		step=1;

	// per-byte reading
	if (MC_ReadByte (mc, adr, &_out)==false)
		return false; // memory read error

	if (_out==0)
		return true; // read OK, empty string

	for (i=0; ; i=i+step)
	{
		if (MC_ReadByte (mc, adr+i, &by)==false)
			return false; // memory read error

		strbuf_addc_C_escaped (out, by, false);
		chars_read++;
		if (by==0)
			break;

		if (my_isprint (by)==false)
		{
			//L (2, __FUNCTION__"() (per-byte read) not a printable string (adr=0x" PRI_ADR_HEX " i=%d, b=0x%x), returning empty string\n", adr, i, b);
			return false;
		};
	};

	return true;
};

bool MC_L_print_buf_in_mem_ofs (MemoryCache *mc, address adr, REG size, REG ofs)
{
	bool rt=false;

	BYTE* buf=DMALLOC (BYTE, size, "buf");

	if (MC_ReadBuffer (mc, adr, size, buf)==false)
		goto exit;

	L_print_buf_ofs (buf, size, ofs); // print starting from zero

	rt=true;
exit:
	DFREE(buf);
	return rt;
};

bool MC_L_print_buf_in_mem (MemoryCache *mc, address adr, SIZE_T size)
{
	return MC_L_print_buf_in_mem_ofs (mc, adr, size, 0);
};

bool MC_get_any_string (MemoryCache *mem, const address adr, strbuf *out)
{
	bool rt=false;
	strbuf t=STRBUF_INIT;

	if (MC_GetString (mem, adr, false, &t))
	{
		strbuf_addc (out, '\"');
		strbuf_addstr (out, t.buf);
		strbuf_addc (out, '\"');
		rt=true;
		goto exit;
	};

	if (MC_GetString (mem, adr, true, &t))
	{
		strbuf_addstr (out, "L\"");
		strbuf_addstr (out, t.buf);
		strbuf_addc (out, '\"');
		rt=true;
		goto exit;
	};

exit:
	strbuf_deinit(&t);
	return rt;
};

bool MC_WriteValue(MemoryCache *mc, address adr, unsigned width, REG val)
{
	switch (width)
	{
		case 1: return MC_WriteByte (mc, adr, val&0xFF);
		case 2: return MC_WriteWyde (mc, adr, val&0xFFFF);
		case 4: return MC_WriteTetrabyte (mc, adr, val&0xFFFFFFFF);
		case 8: return MC_WriteOctabyte (mc, adr, val);
		default:
			oassert(0);
	};
};

