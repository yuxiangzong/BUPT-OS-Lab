/* メモリ関係 */

#include "bootpack.h"

#define EFLAGS_AC_BIT 0x00040000
#define CR0_CACHE_DISABLE 0x60000000
#define CR0_PAGING_BIT 0x80000000
#define PTE_PRESENT 0x001
#define PTE_RW 0x002
#define PDE_ATTR (PTE_PRESENT | PTE_RW | 0x004) /* 0x007 */
#define PTE_ATTR (PTE_PRESENT | PTE_RW | 0x004) /* 0x007 */

unsigned int memtest(unsigned int start, unsigned int end)
{
	char flg486 = 0;
	unsigned int eflg, cr0, i;

	/* 386か、486以降なのかの確認 */
	eflg = io_load_eflags();
	eflg |= EFLAGS_AC_BIT; /* AC-bit = 1 */
	io_store_eflags(eflg);
	eflg = io_load_eflags();
	if ((eflg & EFLAGS_AC_BIT) != 0)
	{ /* 386ではAC=1にしても自動で0に戻ってしまう */
		flg486 = 1;
	}
	eflg &= ~EFLAGS_AC_BIT; /* AC-bit = 0 */
	io_store_eflags(eflg);

	if (flg486 != 0)
	{
		cr0 = load_cr0();
		cr0 |= CR0_CACHE_DISABLE; /* キャッシュ禁止 */
		store_cr0(cr0);
	}

	i = memtest_sub(start, end);

	if (flg486 != 0)
	{
		cr0 = load_cr0();
		cr0 &= ~CR0_CACHE_DISABLE; /* キャッシュ許可 */
		store_cr0(cr0);
	}

	return i;
}

unsigned int init_paging(unsigned int memtotal)
{
	int i, j;
	unsigned int *page_dir = (unsigned int *)PAGE_DIR_ADDR;
	unsigned int *page_table;
	unsigned int num_tables;
	unsigned int cr0;
	unsigned int vram_addr, vram_size;
	int vram_pd_start, vram_pd_end;

	/* 计算需要的页表数量（每个页表映射4MB） */
	num_tables = (memtotal + 0x3fffff) / 0x400000;
	if (num_tables > 128)
	{
		num_tables = 128; /* 最多映射512MB */
	}
	if (num_tables < 1)
	{
		num_tables = 1; /* 至少映射前4MB（内核所在区域） */
	}

	/* 清空页目录 */
	for (i = 0; i < 1024; i++)
	{
		page_dir[i] = 0;
	}

	/* 设置页表，建立一一映射（虚拟地址=物理地址） */
	page_table = (unsigned int *)PAGE_TABLE_ADDR;
	for (i = 0; i < num_tables; i++)
	{
		for (j = 0; j < 1024; j++)
		{
			page_table[j] = (i * 0x400000 + j * 0x1000) | PTE_ATTR;
		}
		page_dir[i] = ((unsigned int)page_table) | PDE_ATTR;
		page_table += 1024; /* 移动到下一个页表 */
	}

	/* 映射VRAM区域（VBE模式下VRAM可能在0xe0000000等高地址） */
	vram_addr = *((unsigned int *)0x0ff8);
	vram_size = (unsigned int)(*((unsigned short *)0x0ff4)) * (unsigned int)(*((unsigned short *)0x0ff6));
	vram_pd_start = (int)(vram_addr >> 22);
	vram_pd_end = (int)((vram_addr + vram_size + 0x3fffff) >> 22);
	for (i = vram_pd_start; i < vram_pd_end; i++)
	{
		if (page_dir[i] == 0)
		{
			page_table = (unsigned int *)(PAGE_TABLE_ADDR + num_tables * 0x1000);
			for (j = 0; j < 1024; j++)
			{
				page_table[j] = (i * 0x400000 + j * 0x1000) | PTE_ATTR;
			}
			page_dir[i] = ((unsigned int)page_table) | PDE_ATTR;
			num_tables++;
		}
	}

	/* 将页目录地址加载到CR3 */
	store_cr3(PAGE_DIR_ADDR);

	/* 设置CR0的bit31，启用分页 */
	cr0 = load_cr0();
	cr0 |= CR0_PAGING_BIT;
	store_cr0(cr0);

	/* 返回页表占用内存的末尾地址 */
	return PAGE_TABLE_ADDR + num_tables * 0x1000;
}

void memman_init(struct MEMMAN *man)
{
	man->frees = 0;	   /* あき情報の個数 */
	man->maxfrees = 0; /* 状況観察用：freesの最大値 */
	man->lostsize = 0; /* 解放に失敗した合計サイズ */
	man->losts = 0;	   /* 解放に失敗した回数 */
	return;
}

unsigned int memman_total(struct MEMMAN *man)
/* あきサイズの合計を報告 */
{
	unsigned int i, t = 0;
	for (i = 0; i < man->frees; i++)
	{
		t += man->free[i].size;
	}
	return t;
}

unsigned int memman_alloc(struct MEMMAN *man, unsigned int size)
/* 分配（Best-Fit 最佳适应算法：选择满足大小要求的最小空闲块） */
{
	unsigned int i, a;
	int best_i = -1;					 /* 最佳块的索引 */
	unsigned int best_size = 0xffffffff; /* 记录最小满足条件的块大小 */

	/* 遍历所有空闲块，找到满足大小要求的最小块 */
	for (i = 0; i < man->frees; i++)
	{
		if (man->free[i].size >= size && man->free[i].size < best_size)
		{
			best_i = i;
			best_size = man->free[i].size;
			if (man->free[i].size == size)
			{
				break;
			}
		}
	}

	if (best_i == -1)
	{
		return 0; /* 没有可用空间 */
	}

	/* 从找到的最佳块中分配 */
	a = man->free[best_i].addr;
	man->free[best_i].addr += size;
	man->free[best_i].size -= size;
	if (man->free[best_i].size == 0)
	{
		/* 如果free[best_i]变成了0，就减掉一条可用信息 */
		man->frees--;
		for (i = best_i; i < man->frees; i++)
		{
			man->free[i] = man->free[i + 1]; /* 代入结构体 */
		}
	}
	return a;
}

int memman_free(struct MEMMAN *man, unsigned int addr, unsigned int size)
/* 解放 */
{
	int i, j;
	/* まとめやすさを考えると、free[]がaddr順に並んでいるほうがいい */
	/* だからまず、どこに入れるべきかを決める */
	for (i = 0; i < man->frees; i++)
	{
		if (man->free[i].addr > addr)
		{
			break;
		}
	}
	/* free[i - 1].addr < addr < free[i].addr */
	if (i > 0)
	{
		/* 前がある */
		if (man->free[i - 1].addr + man->free[i - 1].size == addr)
		{
			/* 前のあき領域にまとめられる */
			man->free[i - 1].size += size;
			if (i < man->frees)
			{
				/* 後ろもある */
				if (addr + size == man->free[i].addr)
				{
					/* なんと後ろともまとめられる */
					man->free[i - 1].size += man->free[i].size;
					/* man->free[i]の削除 */
					/* free[i]がなくなったので前へつめる */
					man->frees--;
					for (; i < man->frees; i++)
					{
						man->free[i] = man->free[i + 1]; /* 構造体の代入 */
					}
				}
			}
			return 0; /* 成功終了 */
		}
	}
	/* 前とはまとめられなかった */
	if (i < man->frees)
	{
		/* 後ろがある */
		if (addr + size == man->free[i].addr)
		{
			/* 後ろとはまとめられる */
			man->free[i].addr = addr;
			man->free[i].size += size;
			return 0; /* 成功終了 */
		}
	}
	/* 前にも後ろにもまとめられない */
	if (man->frees < MEMMAN_FREES)
	{
		/* free[i]より後ろを、後ろへずらして、すきまを作る */
		for (j = man->frees; j > i; j--)
		{
			man->free[j] = man->free[j - 1];
		}
		man->frees++;
		if (man->maxfrees < man->frees)
		{
			man->maxfrees = man->frees; /* 最大値を更新 */
		}
		man->free[i].addr = addr;
		man->free[i].size = size;
		return 0; /* 成功終了 */
	}
	/* 後ろにずらせなかった */
	man->losts++;
	man->lostsize += size;
	return -1; /* 失敗終了 */
}

unsigned int memman_alloc_4k(struct MEMMAN *man, unsigned int size)
{
	unsigned int a;
	size = (size + 0xfff) & 0xfffff000;
	a = memman_alloc(man, size);
	return a;
}

int memman_free_4k(struct MEMMAN *man, unsigned int addr, unsigned int size)
{
	int i;
	size = (size + 0xfff) & 0xfffff000;
	i = memman_free(man, addr, size);
	return i;
}