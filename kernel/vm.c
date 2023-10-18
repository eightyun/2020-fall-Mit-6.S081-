#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
  // 名称以kvm开头的函数操作内核页表；以uvm开头的函数操作用户页表 , ukvm开头的函数操作用户的内核页表
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;   // 它实际上是指向RISC-V根页表页的指针一个pagetable_t可
                               // 可以是内核页表，也可以是一个进程页表

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()  // kvminit以使用 kvmmake创建内核的页表。此调用发生在 xv6 启用 RISC-V 上的分页之前，因此地址直接引用物理内存
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()  //来安装内核页表
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)  // 为虚拟地址找到PTE
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) //kvmmap调用mappages，mappages将范围虚拟地址到同等
{                                      //范围物理地址的映射装载到一个页表中。
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)  // 为新映射装载PTE
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);

  for(;;)
  {
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }

  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{                       // uvmunmap使用walk来查找对应的PTE，并使用kfree来释放PTE引用的物理内存。
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) 
{                   // 用kalloc分配物理内存，并用mappages将PTE添加到用户页表中
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) //复制数据到用户虚拟地址
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) // 从用户虚拟地址复制数据
{                                        // srcva 源虚拟地址
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
   return copyinstr_new(pagetable, dst, srcva, max);
}

void
vmprint_helper(pagetable_t pagetable, int depth) 
{
  static char* indent[] = 
  {
      "",
      "..",
      ".. ..",
      ".. .. .."
  };

  if (depth <= 0 || depth >= 4) 
    panic("vmprint_helper: depth not in {1, 2, 3}");
  
  // there are 2^9 = 512 PTES in a page table.
  for (int i = 0; i < 512; i++) 
  {
    pte_t pte = pagetable[i];

    if (pte & PTE_V) 
    {             //是一个有效的PTE
      printf("%s%d: pte %p pa %p\n", indent[depth], i, pte, PTE2PA(pte));

      if ((pte & (PTE_R|PTE_W|PTE_X)) == 0) 
      {
                  // points to a lower-level page table 并且是间接层PTE
        uint64 child = PTE2PA(pte);
        vmprint_helper((pagetable_t)child, depth+1); // 递归, 深度+1
      }
    }
  }
}

// Utility func to print the valid
// PTEs within a page table recursively
void
vmprint(pagetable_t pagetable) 
{
  printf("page table %p\n", pagetable);
  vmprint_helper(pagetable, 1);
}

// add a mapping to the per-process kernel page table.
void
ukvmmap(pagetable_t pkpagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(pkpagetable, va, sz, pa, perm) != 0)
    panic("ukvmmap");
}

pagetable_t
ukvminit()
{
  pagetable_t pkpagetable = (pagetable_t) kalloc();
  if (pkpagetable == 0) 
    return pkpagetable;
  
  memset(pkpagetable, 0, PGSIZE);
  // 把固定的常数映射照旧搬运过来
  // uart registers
  ukvmmap(pkpagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // virtio mmio disk interface
  ukvmmap(pkpagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // CLINT
  ukvmmap(pkpagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
  // PLIC
  ukvmmap(pkpagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
  // map kernel text executable and read-only.
  ukvmmap(pkpagetable, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);
  // map kernel data and the physical RAM we'll make use of.
  ukvmmap(pkpagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);
  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  ukvmmap(pkpagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  return pkpagetable;
}

// Unmap the leaf node mapping
// of the per-process kernel page table
// so that we could call freewalk on that
void
ukvmunmap(pagetable_t pagetable, uint64 va, uint64 npages)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("ukvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE)
  {
    if((pte = walk(pagetable, a, 0)) == 0)
      goto clean;
    if((*pte & PTE_V) == 0)
      goto clean;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("ukvmunmap: not a leaf");

    clean:
      *pte = 0;
  }
}

// Recursively free page-table pages similar to freewalk
// not need to already free leaf node
// 和freewalk一模一样, 除了不再出panic错当一个page的leaf还没被清除掉
// 因为当我们free pagetable和kpagetable的时候
// 只有1份物理地址, 且原本free pagetable的函数会负责清空它们
// 所以这个函数只需要把在kpagetable里所有间接mapping清除即可
void
ukvmfreewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      ukvmfreewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    pagetable[i] = 0;
  }
  kfree((void*)pagetable);
}

// helper function to first free all leaf mapping
// of a per-process kernel table but do not free the physical address
// and then remove all 3-levels indirection and the physical address
// for this kernel page itself
void 
ukvmfreeproc(struct proc * p) //  在销毁一个进程时, 回收它的内核页表. 这里需要注意的是, 我们并不需要去回收内核页表所映射到的物理地址
{                             // 因为那些物理地址, 例如device mapping, 是全局共享的. 进程专属内核表只是全局内核表的一个复制. 但是间接映射所消耗分配的物理内存是需要回收的
  pagetable_t pkpagetable = p->pkpagetable;
  // reverse order of allocation
  // 按分配顺序的逆序来销毁映射, 但不回收物理地址
  ukvmunmap(pkpagetable, p->kstack, PGSIZE/PGSIZE);
  ukvmunmap(pkpagetable, TRAMPOLINE, PGSIZE/PGSIZE);
  ukvmunmap(pkpagetable, (uint64)etext, (PHYSTOP-(uint64)etext)/PGSIZE);
  ukvmunmap(pkpagetable, KERNBASE, ((uint64)etext-KERNBASE)/PGSIZE);
  ukvmunmap(pkpagetable, PLIC, 0x400000/PGSIZE);
  ukvmunmap(pkpagetable, CLINT, 0x10000/PGSIZE);
  ukvmunmap(pkpagetable, VIRTIO0, PGSIZE/PGSIZE);
  ukvmunmap(pkpagetable, UART0, PGSIZE/PGSIZE);
  ukvmfreewalk(pkpagetable);
}


// help user pagetable to user's kernel pagetable
// 一串helper函数, 来将一段内存映射从pagetable复制到pkpagetable.
int
mappages_u2ukvm(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);

  for(;;)
  {
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;

    *pte = PA2PTE(pa) | perm | PTE_V;

    if(a == last)
      break;

    a += PGSIZE;
    pa += PGSIZE;
  }

  return 0;
}

int
copypage_u2ukvm(pagetable_t pagetable, pagetable_t pkpagetable , uint64 begin , uint64 end)
{
  pte_t * pte ;
  uint64 pa, i;
  uint flags;
  
  begin = PGROUNDUP(begin);

  for(i = begin ; i < end ; i += PGSIZE)
  {
    if((pte = walk(pagetable, i, 0)) == 0)
      panic("copypage_u2ukvm walk pagetable nullptr");

    if((*pte & PTE_V) == 0)
      panic("copypage_u2ukvm walk pte not valid");

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte) & (~PTE_U); // 把U flag抹去  在内核模式下，无法访问设置了PTE_U的页面

    if (mappages_u2ukvm(pkpagetable, i, PGSIZE, pa, flags) != 0) 
      goto err;
  }

  return 0 ;

  err :
    uvmunmap(pkpagetable, 0, i / PGSIZE, 1);
    return -1;
}